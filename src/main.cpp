/**
 * ESP32-S3 Push-to-Talk — Thalamus Chat Live
 * 
 * Board: ESP32S3-AI V2.2 (303ESP32AI2)
 * Chip:  ESP32-S3 N16R8 (16MB Flash, 8MB OPI PSRAM)
 * 
 * Confirmed pin mapping:
 *   INMP441 Mic:   WS=GPIO4, SCK=GPIO5, SD=GPIO6    (I2S_NUM_1, 16kHz RX)
 *   MAX98357 Spk:  DIN=GPIO7, BCLK=GPIO15, LRCK=GPIO16 (I2S_NUM_0, 24kHz TX)
 *   Buttons:       BOOT=GPIO0 (PTT), VOL-=GPIO39, VOL+=GPIO40
 *   LED:           GPIO48
 * 
 * Protocol:
 *   - Connect WSS to Thalamus /chat/live
 *   - Send: raw PCM16 16kHz mono binary frames while PTT held
 *   - Receive: raw PCM16 24kHz mono binary frames → play on speaker
 *   - Receive: JSON text events (transcript, turn_complete, interrupted, etc.)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <esp_wifi.h>

// ═══════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════

const char* WIFI_SSID     = "Sum Vivas";
const char* WIFI_PASS     = "34032039";

const char* THALAMUS_HOST = "dev-thalamus-next.sumvivas.com";
const uint16_t THALAMUS_PORT = 443;
const char* THALAMUS_PATH = "/chat/live?template_id=Aimee_kisok_demo&session_id=esp32-aitoy-001";

// ═══════════════════════════════════════════════════════════════
//  Pin Definitions (confirmed by hardware test)
// ═══════════════════════════════════════════════════════════════

// INMP441 Microphone — I2S_NUM_1 (16 kHz RX)
// Proven working: WS=4, SCK=5, SD=6, ONLY_LEFT  (L/R tied to GND)
#define MIC_I2S_PORT    I2S_NUM_1
#define MIC_WS_PIN      4
#define MIC_SCK_PIN     5
#define MIC_SD_PIN      6

// MAX98357 Speaker — I2S_NUM_0 (24 kHz TX)
#define SPK_I2S_PORT    I2S_NUM_0
#define SPK_DIN_PIN     7
#define SPK_BCLK_PIN    15
#define SPK_LRCK_PIN    16

// Buttons
// NOTE: GPIO47 (WiFi_RST) is wired to EN/reset — pressing it reboots the chip!
// Using GPIO0 (BOOT button) as PTT instead. CH340X may hold it LOW when serial
// port is open with DTR asserted, but monitor.py sets DTR=False so it works.
#define PTT_PIN         GPIO_NUM_0   // BOOT button = Push-to-Talk
#define VOL_DOWN_PIN    GPIO_NUM_39
#define VOL_UP_PIN      GPIO_NUM_40

// LED
#define LED_PIN         GPIO_NUM_48

// ═══════════════════════════════════════════════════════════════
//  Audio Parameters
// ═══════════════════════════════════════════════════════════════

#define MIC_SAMPLE_RATE     16000
#define SPK_SAMPLE_RATE     24000

// 20ms of 16kHz 16-bit mono = 320 samples = 640 bytes
#define MIC_BUF_SAMPLES     320
#define MIC_BUF_BYTES       (MIC_BUF_SAMPLES * 2)

// Playback ring buffer — 1 second of 24kHz audio in PSRAM
#define PLAY_RING_BYTES     (SPK_SAMPLE_RATE * 2 * 1)  // 48000 bytes = 1s
uint8_t* playRing = nullptr;
volatile size_t playHead = 0;
volatile size_t playTail = 0;

// Volume control (0-10 scale)
volatile int volumeLevel = 7;  // default ~70%

// Mic gain — INMP441 output is very weak on this board (~841/32767 peak)
// Multiply raw samples by this factor before sending
#define MIC_GAIN        20

// ═══════════════════════════════════════════════════════════════
//  State
// ═══════════════════════════════════════════════════════════════

WebSocketsClient ws;
volatile bool wsConnected = false;
volatile bool recording   = false;
volatile bool playing     = false;

// Session info
char sessionId[64] = {0};
int turnCount = 0;

// LED state
enum LedState {
    LED_OFF,
    LED_WIFI_CONNECTING,   // slow blink
    LED_WS_CONNECTING,     // fast blink
    LED_IDLE,              // solid dim
    LED_RECORDING,         // solid bright
    LED_PLAYING,           // pulsing
    LED_ERROR              // rapid flash
};
volatile LedState ledState = LED_OFF;

// ═══════════════════════════════════════════════════════════════
//  I2S Setup
// ═══════════════════════════════════════════════════════════════

void setupMicI2S() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = MIC_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;  // L/R pin tied to GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = MIC_BUF_SAMPLES;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = MIC_SCK_PIN;
    pins.ws_io_num    = MIC_WS_PIN;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = MIC_SD_PIN;

    ESP_ERROR_CHECK(i2s_driver_install(MIC_I2S_PORT, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(MIC_I2S_PORT, &pins));
    i2s_zero_dma_buffer(MIC_I2S_PORT);
    Serial.println("[I2S] Mic initialized (16kHz, I2S_NUM_1, LEFT channel)");
}

void setupSpeakerI2S() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SPK_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = SPK_BCLK_PIN;
    pins.ws_io_num    = SPK_LRCK_PIN;
    pins.data_out_num = SPK_DIN_PIN;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(i2s_driver_install(SPK_I2S_PORT, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(SPK_I2S_PORT, &pins));
    i2s_zero_dma_buffer(SPK_I2S_PORT);
    Serial.println("[I2S] Speaker initialized (24kHz, I2S_NUM_0)");
}

// ═══════════════════════════════════════════════════════════════
//  Ring Buffer Helpers
// ═══════════════════════════════════════════════════════════════

inline size_t ringAvailable() {
    return (playHead - playTail + PLAY_RING_BYTES) % PLAY_RING_BYTES;
}

void ringWrite(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        playRing[playHead] = data[i];
        playHead = (playHead + 1) % PLAY_RING_BYTES;
    }
}

size_t ringRead(uint8_t* buf, size_t len) {
    size_t avail = ringAvailable();
    size_t toRead = min(len, avail);
    for (size_t i = 0; i < toRead; i++) {
        buf[i] = playRing[playTail];
        playTail = (playTail + 1) % PLAY_RING_BYTES;
    }
    return toRead;
}

void ringFlush() {
    playHead = 0;
    playTail = 0;
}

// ═══════════════════════════════════════════════════════════════
//  Volume Control — apply to 16-bit PCM buffer
// ═══════════════════════════════════════════════════════════════

void applyVolume(int16_t* samples, size_t count) {
    if (volumeLevel >= 10) return;  // max volume, no change
    if (volumeLevel <= 0) {
        memset(samples, 0, count * 2);
        return;
    }
    float scale = volumeLevel / 10.0f;
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(samples[i] * scale);
    }
}

// ═══════════════════════════════════════════════════════════════
//  WebSocket Event Handler
// ═══════════════════════════════════════════════════════════════

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

    case WStype_CONNECTED:
        Serial.println("[WS] Connected to Thalamus!");
        wsConnected = true;
        ledState = LED_IDLE;
        break;

    case WStype_DISCONNECTED:
        Serial.println("[WS] Disconnected from Thalamus");
        wsConnected = false;
        ledState = LED_WS_CONNECTING;
        break;

    case WStype_BIN:
        // Received 24kHz PCM16 audio from assistant → ring buffer
        ringWrite(payload, length);
        playing = true;
        break;

    case WStype_TEXT: {
        // Parse JSON event
        Serial.printf("[WS] %.*s\n", (int)min(length, (size_t)300), payload);

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err) {
            Serial.printf("[WS] JSON parse error: %s\n", err.c_str());
            break;
        }

        const char* eventType = doc["type"];
        if (!eventType) break;

        if (strcmp(eventType, "session_started") == 0) {
            const char* sid = doc["session_id"];
            if (sid) strncpy(sessionId, sid, sizeof(sessionId) - 1);
            Serial.printf("[Session] Started: %s\n", sessionId);
        }
        else if (strcmp(eventType, "transcript") == 0) {
            const char* role = doc["role"];
            const char* text = doc["text"];
            Serial.printf("[%s] %s\n", role ? role : "?", text ? text : "");
        }
        else if (strcmp(eventType, "turn_complete") == 0) {
            turnCount = doc["turn"] | turnCount;
            Serial.printf("[Turn] %d complete\n", turnCount);
        }
        else if (strcmp(eventType, "interrupted") == 0) {
            Serial.println("[WS] Barge-in — flushing playback");
            ringFlush();
            i2s_zero_dma_buffer(SPK_I2S_PORT);
            playing = false;
        }
        else if (strcmp(eventType, "session_ended") == 0) {
            int turns = doc["turns"] | 0;
            int tokens = doc["tokens"] | 0;
            Serial.printf("[Session] Ended — %d turns, %d tokens\n", turns, tokens);
        }
        else if (strcmp(eventType, "error") == 0) {
            const char* msg = doc["message"];
            Serial.printf("[ERROR] %s\n", msg ? msg : "unknown");
            ledState = LED_ERROR;
        }
        break;
    }

    case WStype_ERROR:
        Serial.printf("[WS] Error: %.*s\n", (int)length, payload);
        ledState = LED_ERROR;
        break;

    case WStype_PING:
        Serial.println("[WS] Ping");
        break;

    case WStype_PONG:
        break;

    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Playback Task — runs on Core 0
// ═══════════════════════════════════════════════════════════════

void playbackTask(void* param) {
    const size_t CHUNK = 512;  // bytes per I2S write
    uint8_t buf[CHUNK];
    size_t written;

    Serial.println("[Playback] Task started on core 0");

    for (;;) {
        size_t avail = ringAvailable();

        if (avail >= CHUNK) {
            ringRead(buf, CHUNK);
            applyVolume((int16_t*)buf, CHUNK / 2);
            i2s_write(SPK_I2S_PORT, buf, CHUNK, &written, portMAX_DELAY);
        } else if (avail > 0 && avail < CHUNK) {
            // Partial buffer — pad with silence
            size_t got = ringRead(buf, avail);
            applyVolume((int16_t*)buf, got / 2);
            memset(buf + got, 0, CHUNK - got);
            i2s_write(SPK_I2S_PORT, buf, CHUNK, &written, portMAX_DELAY);
            playing = false;
        } else {
            // No audio — write silence to keep DMA happy
            if (playing) {
                playing = false;
                if (ledState == LED_PLAYING) ledState = LED_IDLE;
            }
            memset(buf, 0, CHUNK);
            i2s_write(SPK_I2S_PORT, buf, CHUNK, &written, portMAX_DELAY);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  LED Task — visual feedback
// ═══════════════════════════════════════════════════════════════

void ledTask(void* param) {
    pinMode(LED_PIN, OUTPUT);
    int counter = 0;

    for (;;) {
        counter++;
        switch (ledState) {
            case LED_OFF:
                digitalWrite(LED_PIN, LOW);
                break;
            case LED_WIFI_CONNECTING:
                // ~1s blink cycle
                digitalWrite(LED_PIN, (counter % 20) < 10 ? HIGH : LOW);
                break;
            case LED_WS_CONNECTING:
                // fast blink
                digitalWrite(LED_PIN, (counter % 6) < 3 ? HIGH : LOW);
                break;
            case LED_IDLE:
                // brief pulse every 2s
                digitalWrite(LED_PIN, (counter % 40) < 2 ? HIGH : LOW);
                break;
            case LED_RECORDING:
                digitalWrite(LED_PIN, HIGH);  // solid on
                break;
            case LED_PLAYING:
                // quick pulse
                digitalWrite(LED_PIN, (counter % 4) < 2 ? HIGH : LOW);
                break;
            case LED_ERROR:
                // rapid flash
                digitalWrite(LED_PIN, (counter % 4) < 2 ? HIGH : LOW);
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═══════════════════════════════════════════════════════════════
//  Button Handling
// ═══════════════════════════════════════════════════════════════

void handleVolumeButtons() {
    static bool volUpPrev = HIGH, volDownPrev = HIGH;
    static unsigned long lastVolChange = 0;

    if (millis() - lastVolChange < 200) return;  // debounce

    bool volUp = digitalRead(VOL_UP_PIN);
    bool volDown = digitalRead(VOL_DOWN_PIN);

    if (volUp == LOW && volUpPrev == HIGH) {
        volumeLevel = min(10, volumeLevel + 1);
        Serial.printf("[Volume] %d/10\n", volumeLevel);
        lastVolChange = millis();
    }
    if (volDown == LOW && volDownPrev == HIGH) {
        volumeLevel = max(0, volumeLevel - 1);
        Serial.printf("[Volume] %d/10\n", volumeLevel);
        lastVolChange = millis();
    }

    volUpPrev = volUp;
    volDownPrev = volDown;
}

// ═══════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("==========================================");
    Serial.println(" ESP32-S3 Push-to-Talk — Thalamus Live");
    Serial.println(" Board: ESP32S3-AI V2.2 (N16R8)");
    Serial.println("==========================================");
    Serial.printf(" Chip: %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf(" PSRAM: %d bytes\n", ESP.getPsramSize());
    Serial.printf(" Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println();

    // Allocate ring buffer in PSRAM if available
    if (ESP.getPsramSize() > 0) {
        playRing = (uint8_t*)ps_malloc(PLAY_RING_BYTES);
        Serial.printf("[Mem] Ring buffer: %d bytes in PSRAM\n", PLAY_RING_BYTES);
    } else {
        playRing = (uint8_t*)malloc(PLAY_RING_BYTES);
        Serial.printf("[Mem] Ring buffer: %d bytes in SRAM\n", PLAY_RING_BYTES);
    }
    if (!playRing) {
        Serial.println("[FATAL] Could not allocate ring buffer!");
        while (1) delay(1000);
    }
    memset(playRing, 0, PLAY_RING_BYTES);

    // Buttons
    pinMode(PTT_PIN, INPUT_PULLUP);
    pinMode(VOL_DOWN_PIN, INPUT_PULLUP);
    pinMode(VOL_UP_PIN, INPUT_PULLUP);

    // Check if BOOT button (GPIO0) is stuck LOW from CH340X
    delay(100);
    int gpio0state = digitalRead(PTT_PIN);
    Serial.printf("[BTN] BOOT/PTT (GPIO0) = %s\n", gpio0state == HIGH ? "HIGH (OK)" : "LOW (stuck!)");
    if (gpio0state == LOW) {
        Serial.println("[BTN] WARNING: GPIO0 held LOW by USB chip — PTT may not work.");
        Serial.println("[BTN]   Try: close other serial tools, or disconnect/reconnect USB.");
    }

    // LED task
    xTaskCreatePinnedToCore(ledTask, "led", 2048, NULL, 1, NULL, 0);
    ledState = LED_WIFI_CONNECTING;

    // ── Wi-Fi ──────────────────────────────────────────────
    Serial.printf("[WiFi] Connecting to \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        attempts++;
        if (attempts > 40) {  // 20 seconds timeout
            Serial.println("\n[WiFi] FAILED to connect!");
            ledState = LED_ERROR;
            break;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());

        // Disable WiFi power saving for lowest latency
        esp_wifi_set_ps(WIFI_PS_NONE);
    }

    // ── I2S ────────────────────────────────────────────────
    setupMicI2S();
    setupSpeakerI2S();

    // Flush mic buffer (discard initial noise)
    {
        int16_t flush[512];
        size_t br;
        for (int i = 0; i < 10; i++) {
            i2s_read(MIC_I2S_PORT, flush, sizeof(flush), &br, 50);
        }
    }

    // ── Mic diagnostic dump ────────────────────────────────
    {
        Serial.println("[DIAG] Mic quick check...");
        int16_t diag[320];
        size_t br;
        i2s_read(MIC_I2S_PORT, diag, sizeof(diag), &br, pdMS_TO_TICKS(100));
        int16_t peak = 0;
        for (int i = 0; i < (int)(br / 2); i++) {
            int16_t v = diag[i] < 0 ? -diag[i] : diag[i];
            if (v > peak) peak = v;
        }
        Serial.printf("[DIAG] Mic peak=%d  bytes=%d  %s\n",
            peak, br, peak > 50 ? "OK" : (peak > 0 ? "WEAK" : "DEAD"));
    }

    // ── WebSocket ──────────────────────────────────────────
    ledState = LED_WS_CONNECTING;
    Serial.printf("[WS] Connecting to wss://%s%s\n", THALAMUS_HOST, THALAMUS_PATH);

    ws.beginSSL(THALAMUS_HOST, THALAMUS_PORT, THALAMUS_PATH);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(3000);

    // ── Playback task on core 0 ────────────────────────────
    xTaskCreatePinnedToCore(playbackTask, "playback", 4096, NULL, 2, NULL, 0);

    Serial.println();
    Serial.println("==========================================");
    Serial.println(" Ready! Hold BOOT button to talk.");
    Serial.println(" VOL+/VOL- to adjust volume.");
    Serial.printf(" Mic gain: %dx\n", MIC_GAIN);
    Serial.println("==========================================");
    Serial.println();
}

// ═══════════════════════════════════════════════════════════════
//  Main Loop — Core 1
// ═══════════════════════════════════════════════════════════════

void loop() {
    // Service WebSocket
    ws.loop();

    // Handle volume buttons
    handleVolumeButtons();

    // Push-to-Talk
    bool pttPressed = (digitalRead(PTT_PIN) == LOW);

    if (pttPressed && wsConnected) {
        if (!recording) {
            Serial.println("[PTT] >> Recording started");
            recording = true;
            playing = false;
            ledState = LED_RECORDING;

            // Flush any stale audio in playback buffer — stop speaker to prevent echo
            ringFlush();
            i2s_zero_dma_buffer(SPK_I2S_PORT);
        }

        // Read 20ms of mic audio (320 samples @ 16kHz = 640 bytes)
        int16_t micBuf[MIC_BUF_SAMPLES];
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(MIC_I2S_PORT, micBuf, MIC_BUF_BYTES,
                                 &bytesRead, pdMS_TO_TICKS(25));

        if (err == ESP_OK && bytesRead > 0) {
            // Debug: log peak signal level every ~500ms
            static unsigned long lastMicLog = 0;
            static int micLogCount = 0;
            micLogCount++;
            if (millis() - lastMicLog > 500) {
                int16_t peak = 0;
                int numSamplesDbg = bytesRead / sizeof(int16_t);
                for (int i = 0; i < numSamplesDbg; i++) {
                    int16_t v = micBuf[i] < 0 ? -micBuf[i] : micBuf[i];
                    if (v > peak) peak = v;
                }
                Serial.printf("[MIC] peak=%d  bytes=%d  frames=%d\n",
                    peak, bytesRead, micLogCount);
                lastMicLog = millis();
                micLogCount = 0;
            }

            // Apply mic gain — INMP441 signal is weak without amplification
            int numSamples = bytesRead / sizeof(int16_t);
            for (int i = 0; i < numSamples; i++) {
                int32_t amplified = (int32_t)micBuf[i] * MIC_GAIN;
                if (amplified > 32767) amplified = 32767;
                else if (amplified < -32768) amplified = -32768;
                micBuf[i] = (int16_t)amplified;
            }
            // Send raw PCM16 as binary WebSocket frame
            ws.sendBIN((uint8_t*)micBuf, bytesRead);
        } else if (err != ESP_OK) {
            Serial.printf("[MIC] i2s_read error: %d\n", err);
        }

    } else {
        if (recording) {
            Serial.println("[PTT] << Recording stopped");
            recording = false;
            ledState = ringAvailable() > 0 ? LED_PLAYING : LED_IDLE;
        }
    }

    // Update LED state based on playback
    if (!recording && playing && ledState != LED_PLAYING) {
        ledState = LED_PLAYING;
    }
    if (!recording && !playing && ledState == LED_PLAYING) {
        ledState = LED_IDLE;
    }

    // NOTE: GPIO47 (WiFi_RST) is wired to EN — do NOT use as input (causes reset)
}
