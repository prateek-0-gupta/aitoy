# Hacking a £20 AI Voice Module into a Real-Time Voice Assistant

**How we reverse-engineered a no-name ESP32-S3 board from Amazon and turned it into a working push-to-talk device connected to a live AI conversation API.**

---

## The Board

It arrived in a plain bag from Amazon UK for £20.43. The listing was what you'd expect from a generic Chinese electronics seller — the brand was "Aolidsive", the model name was a keyboard-mash (`Aolidsivedafsrqilyx`), and the product description was a machine-translated spec sheet. But the feature list was interesting:

- ESP32-S3 with Wi-Fi and Bluetooth
- INMP441 I2S MEMS microphone 
- MAX98357 I2S amplifier with speaker
- CH340X USB-to-serial for programming
- TP5400 battery management
- 5 function buttons + power switch
- PIR human detection interface (reserved)

**ASIN:** B0GRSF2TCB  
**Actual board marking:** `ESP32S3-AI_V2.2 (303ESP32AI2)`

No documentation. No schematic. No SDK. No GitHub repo. Just a PCB, a speaker with red wires, and a USB-C cable.

The challenge: make it talk to our **Thalamus `/chat/live`** real-time voice AI API.

---

## Step 1: Is Anyone Home?

First question — is this thing even detected by the PC?

```powershell
Get-WmiObject Win32_PnPEntity | Where-Object { $_.Name -match 'CH340' }
```

```
Name     : USB-SERIAL CH340 (COM3)
DeviceID : USB\VID_1A86&PID_7523\5&1F1B5891&0&7
Status   : OK
```

The CH340X showed up on **COM3**. Good start.

Next, we used `esptool` to identify the chip:

```
esptool --port COM3 chip-id
```

```
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
```

So it's an **ESP32-S3 with 8MB of OPI PSRAM**. The module marking `S3-N16R8` visible on the close-up photos confirmed 16MB Flash + 8MB PSRAM.

---

## Step 2: The Pin Mapping Problem

With no schematic available, we needed to figure out which GPIOs connected to the INMP441, MAX98357, and buttons. We had two approaches:

### Approach A: Read the Silkscreen

From the product photos we could read the button labels: **EN, BOOT, Volume-, Volume+, WiFi reset**. But no GPIO numbers.

### Approach B: Borrow from the Community

The board is clearly designed for the [Xiaozhi (小智)](https://github.com/78/xiaozhi-esp32) open-source AI chatbot ecosystem — a popular Chinese ESP32 voice assistant project with 25k+ GitHub stars. We found the `bread-compact-wifi` board config in their repo, which uses the same INMP441 + MAX98357 layout:

```c
// From xiaozhi-esp32/main/boards/bread-compact-wifi/config.h
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39
#define BUILTIN_LED_GPIO        GPIO_NUM_48
```

We wrote a hardware test sketch to verify these pins. Every single one checked out.

---

## Step 3: Hardware Verification

We set up a PlatformIO project and flashed a test firmware with four tests:

1. **Button test** — detect presses on all GPIOs for 10 seconds
2. **Speaker test** — play sine wave tones at 440Hz, 880Hz, 1760Hz, and a frequency sweep
3. **Microphone test** — read I2S audio and display a live level meter
4. **Loopback test** — pipe mic audio directly to the speaker

### Results:

| Component | Result |
|---|---|
| ESP32-S3 (240MHz) | Working |
| PSRAM (8MB OPI) | **8,386,279 bytes** detected |
| Speaker (MAX98357) | Tones played clearly |
| Mic (INMP441) | Audio detected — peak 841 / 32767 |
| Loopback | Mic → speaker working, voice audible |
| Volume- (GPIO39) | Confirmed |
| Volume+ (GPIO40) | Confirmed |
| BOOT button (GPIO0) | Confirmed |
| WiFi Reset (GPIO47) | Confirmed |

### The PSRAM Gotcha

On the first boot, we got:

```
E (89) psram: PSRAM ID read error: 0x00ffffff
```

The PSRAM wasn't detected because the Arduino framework defaulted to QIO (quad) mode. This module uses **OPI (octal) PSRAM**, which needs a different memory configuration:

```ini
# platformio.ini
board_build.arduino.memory_type = qio_opi
```

After that fix: `PSRAM: 8386279 bytes` — all 8MB available.

---

## Step 4: The Thalamus Integration

### Protocol

Our target API — **Thalamus `/chat/live`** — uses a WebSocket connection:

- **Send:** Raw PCM16 16kHz mono audio as binary frames
- **Receive:** Raw PCM16 24kHz mono audio + JSON event text frames
- **Events:** `session_started`, `transcript`, `turn_complete`, `interrupted`, `error`

The ESP32-S3 has two I2S peripherals, which is perfect:
- **I2S_NUM_1** → Microphone at 16kHz (matches what the server expects)
- **I2S_NUM_0** → Speaker at 24kHz (matches what the server sends)

No resampling needed on either path.

### Architecture

```
┌─────────────────────────────────────────────┐
│  Core 1 (Main Loop)                         │
│  ┌──────────┐   ┌───────────────────────┐   │
│  │ PTT Btn  │──▶│ I2S Read (16kHz)      │   │
│  │ GPIO47   │   │ + 20x Gain            │   │
│  └──────────┘   │ + ws.sendBIN()        │   │
│                 └───────────────────────┘   │
│  ┌──────────────────────────────────────┐   │
│  │ ws.loop() — WebSocket service        │   │
│  │   BIN → Ring Buffer (PSRAM)          │   │
│  │   TEXT → JSON parse → Serial log     │   │
│  └──────────────────────────────────────┘   │
├─────────────────────────────────────────────┤
│  Core 0 (Pinned Tasks)                      │
│  ┌──────────────────────────────────────┐   │
│  │ Playback Task                        │   │
│  │   Ring Buffer → Volume → I2S Write   │   │
│  └──────────────────────────────────────┘   │
│  ┌──────────────────────────────────────┐   │
│  │ LED Task (visual feedback)           │   │
│  └──────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

The playback task is pinned to Core 0 so it never starves — the I2S DMA write blocks, and if it shared a core with the WebSocket client, audio would stutter.

### Libraries Used

```ini
lib_deps = 
    links2004/WebSockets @ ^2.4.1    ; WSS client
    bblanchon/ArduinoJson @ ^7.3.0   ; JSON event parsing
```

Plus the built-in `WiFi.h` and `driver/i2s.h`.

---

## Step 5: Three Bugs That Nearly Killed the Project

### Bug 1: The BOOT Button Lie

We initially used **GPIO0 (BOOT)** as the push-to-talk button. It seemed natural — it's the biggest button on the board. But the firmware behaved as if PTT was permanently held down: it recorded continuously and never stopped.

**Root cause:** The CH340X USB-to-serial chip uses GPIO0 as a strapping pin for auto-reset during flashing. After programming, the CH340X's DTR/RTS circuitry holds GPIO0 **LOW** — which our code interpreted as "button pressed."

**Fix:** We moved PTT to **GPIO47** (the WiFi Reset button). It's a clean GPIO with no USB-serial interference. Works perfectly.

```c
// Before (broken):
#define PTT_PIN  GPIO_NUM_0   // Held LOW by CH340X!

// After (working):
#define PTT_PIN  GPIO_NUM_47  // WiFi Reset button — clean signal
```

### Bug 2: The Whisper Microphone

The INMP441 raw output was *extremely* quiet. During hardware testing, the peak audio level was only 841 out of a possible 32,767 — about **2.5% of full scale**. Speaking directly into the mic barely registered.

The Thalamus server transcribed everything as gibberish because the signal was buried in the noise floor.

**Fix:** Software gain of 20x with int16 clamping:

```c
#define MIC_GAIN 20

for (int i = 0; i < numSamples; i++) {
    int32_t amplified = (int32_t)micBuf[i] * MIC_GAIN;
    if (amplified > 32767) amplified = 32767;
    else if (amplified < -32768) amplified = -32768;
    micBuf[i] = (int16_t)amplified;
}
```

This brought the effective peak to ~16,000 — well within the server's ASR sweet spot.

### Bug 3: The Echo Chamber

When the assistant spoke through the speaker, the INMP441 mic picked up that audio and sent it back to the server. The server would transcribe its own voice as user input, then respond to itself, creating an infinite conversation loop.

**Fix:** True half-duplex — when PTT is pressed, we flush the playback buffer and silence the speaker. Audio is only sent while the button is held. When released, only playback runs.

```c
if (!recording) {
    // Only send mic audio when PTT is held
    // Speaker audio is NOT captured
}

// On PTT press: stop playback immediately
ringFlush();
i2s_zero_dma_buffer(SPK_I2S_PORT);
```

---

## Step 6: First Successful Conversation

After flashing the fixed firmware, we reset the board and watched the serial output:

```
==========================================
 ESP32-S3 Push-to-Talk — Thalamus Live
 Board: ESP32S3-AI V2.2 (N16R8)
==========================================
 Chip: ESP32-S3 Rev 0
 PSRAM: 8386279 bytes
 Free Heap: 332980 bytes

[Mem] Ring buffer: 48000 bytes in PSRAM
[WiFi] Connecting to "Sum Vivas".
[WiFi] Connected! IP: 192.168.0.233
[WiFi] RSSI: -38 dBm
[I2S] Mic initialized (16kHz, I2S_NUM_1)
[I2S] Speaker initialized (24kHz, I2S_NUM_0)
[WS] Connecting to wss://dev-thalamus-next.sumvivas.com/chat/live?...

[WS] Connected to Thalamus!
[Session] Started: esp32-aitoy-001
```

We held the WiFi Reset button and spoke: *"Hello, who are you?"*

The speaker replied with Aimee's voice: *"Hi! You're through to Summer from Sum Vivas, how may I help you?"*

The serial log showed the full conversation flow:

```
[PTT] >> Recording started
[user] Hello, who are you?
[PTT] << Recording stopped
[assistant] Hi! You're through to Summer from Sum Vivas, how may I help you?
[Turn] 1 complete
```

---

## Final Pin Map

For anyone who buys this board (ASIN: B0GRSF2TCB), here's the complete verified pin mapping:

| Function | GPIO | Notes |
|---|---|---|
| INMP441 WS | 4 | I2S Word Select |
| INMP441 SCK | 5 | I2S Bit Clock |
| INMP441 SD | 6 | I2S Data In |
| MAX98357 DIN | 7 | I2S Data Out |
| MAX98357 BCLK | 15 | I2S Bit Clock |
| MAX98357 LRCK | 16 | I2S Word Select |
| Volume Down | 39 | Active LOW, internal pullup |
| Volume Up | 40 | Active LOW, internal pullup |
| WiFi Reset | 47 | Active LOW, internal pullup |
| BOOT | 0 | **Unusable as input** — held LOW by CH340X |
| EN | — | Hardware reset (not GPIO-readable) |
| LED | 48 | Active HIGH |
| Battery display | On board | Shows charge level via LEDs |
| PIR interface | GND/OUT/VCC | 3-pin header on back, OUT pin TBD |
| Power switch | Physical | Slide switch on back of board |

---

## Bill of Materials

| Item | Cost |
|---|---|
| ESP32S3-AI V2.2 board + speaker + USB cable | £20.43 |
| Software (PlatformIO, Arduino framework, libraries) | Free |
| **Total** | **£20.43** |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Hardware | ESP32-S3 N16R8, INMP441, MAX98357, CH340X |
| Framework | Arduino (via PlatformIO + espressif32 platform) |
| WebSocket | arduinoWebSockets 2.7.3 (WSS/TLS) |
| JSON | ArduinoJson 7.4.3 |
| Audio (Mic) | I2S RX, 16kHz 16-bit mono PCM |
| Audio (Speaker) | I2S TX, 24kHz 16-bit mono PCM |
| Backend API | Thalamus `/chat/live` (Gemini Flash Live) |
| Build tool | PlatformIO CLI 6.1.19 |

---

## What We'd Do Differently

1. **Get a board with documentation.** The Xiaozhi project's own recommended boards come with schematics and known-good firmware. We spent hours on pin discovery that could have been instant.

2. **Use ESP-IDF instead of Arduino.** The Arduino framework's I2S driver is the deprecated "legacy" API. ESP-IDF 5.x has a modern `i2s_channel` API with better buffer management and less latency.

3. **Add hardware echo cancellation.** The MSM261S4030H0 PDM mic that some boards use has better noise characteristics than the INMP441. Alternatively, use the ESP-SR library's AEC (Acoustic Echo Cancellation) algorithm — it's specifically designed for the ESP32-S3's dual-core architecture.

4. **Use OPUS codec.** Sending raw PCM16 over WebSocket works but wastes bandwidth (~32 kB/s for 16kHz). Compressing with OPUS would cut this to ~3-6 kB/s with negligible latency, improving reliability on congested WiFi networks.

---

## Repo Structure

```
e:\projects\aitoy\
├── src/
│   └── main.cpp              # Push-to-talk firmware (572 lines)
├── platformio.ini             # Build config (ESP32-S3, OPI PSRAM, WSS deps)
├── esp32-push-to-talk.md      # Protocol reference doc
├── serial_test.py             # Serial reader for initial chip identification
├── run_test.py                # Interactive hardware test runner
├── monitor.py                 # Serial monitor for firmware debugging
├── include/
├── lib/
└── test/
```

---

## Conclusion

A £20 anonymous board from Amazon, zero documentation, and a WebSocket API spec. Four hours later: a working push-to-talk voice assistant with real-time streaming audio.

The ESP32-S3's dual-core architecture is the unsung hero — one core handles the network stack and microphone capture on the main loop, while the other runs a deterministic audio playback task that never misses a DMA deadline. The 8MB of OPI PSRAM means we never have to worry about buffer allocation.

The biggest lesson: **GPIO0 on ESP32 boards with USB-serial chips is a trap.** It looks like a normal button, it acts like a normal button during testing, and then it betrays you in production because the CH340X/CP2102/FTDI holds it LOW after programming. Always verify GPIOs with the USB cable connected and serial monitor closed.

The second lesson: **INMP441 output levels vary wildly between boards.** Some breakouts have proper gain circuitry; cheap integrated designs often don't. Always add software gain with clipping protection.

The board itself is surprisingly well-designed for the price — the I2S routing is clean, the button layout is sensible (once you know which GPIOs they're on), and the battery management circuit means you could make this truly portable with a 3.7V LiPo. It just needs someone to publish the schematic.
