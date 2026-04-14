/**
 * INMP441 Microphone Diagnostic — ESP32S3-AI V2.2
 * 
 * Tests ALL combinations of I2S port, pin wiring, and channel format.
 * NO WiFi, NO speaker, NO WebSocket — just the mic and serial output.
 * 
 * This will definitively show which config produces audio signal.
 */

#include <Arduino.h>
#include <driver/i2s.h>

#define LED_PIN  GPIO_NUM_48

// Two pin wiring options to test
struct PinConfig {
    const char* name;
    int ws, sck, sd;
};

PinConfig pinConfigs[] = {
    {"WS=4 SCK=5 SD=6", 4, 5, 6},    // Original from Xiaozhi / article
    {"WS=5 SCK=4 SD=6", 5, 4, 6},    // Swapped WS/SCK
};

// Channel formats to test
struct ChanConfig {
    const char* name;
    i2s_channel_fmt_t fmt;
};

ChanConfig chanConfigs[] = {
    {"ONLY_LEFT",    I2S_CHANNEL_FMT_ONLY_LEFT},
    {"ONLY_RIGHT",   I2S_CHANNEL_FMT_ONLY_RIGHT},
    {"RIGHT_LEFT",   I2S_CHANNEL_FMT_RIGHT_LEFT},
};

// I2S ports to test
i2s_port_t ports[] = {I2S_NUM_0, I2S_NUM_1};
const char* portNames[] = {"I2S_NUM_0", "I2S_NUM_1"};

void testConfig(i2s_port_t port, const char* portName,
                PinConfig& pins, ChanConfig& chan) {
    
    Serial.printf("\n--- Testing: %s | %s | %s ---\n", 
                  portName, pins.name, chan.name);

    // Configure I2S
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = 16000;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = chan.fmt;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 320;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;

    i2s_pin_config_t pinCfg = {};
    pinCfg.bck_io_num   = pins.sck;
    pinCfg.ws_io_num    = pins.ws;
    pinCfg.data_out_num = I2S_PIN_NO_CHANGE;
    pinCfg.data_in_num  = pins.sd;

    esp_err_t err;
    
    err = i2s_driver_install(port, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("  i2s_driver_install FAILED: %d\n", err);
        return;
    }

    err = i2s_set_pin(port, &pinCfg);
    if (err != ESP_OK) {
        Serial.printf("  i2s_set_pin FAILED: %d\n", err);
        i2s_driver_uninstall(port);
        return;
    }

    // Flush initial data
    int16_t flush[512];
    size_t br;
    for (int i = 0; i < 5; i++) {
        i2s_read(port, flush, sizeof(flush), &br, 50);
    }

    // Read 5 rounds and report peak/avg
    int16_t buf[640];  // enough for stereo: 320 * 2
    int overallPeak = 0;
    long overallSum = 0;
    int totalSamples = 0;

    for (int round = 0; round < 5; round++) {
        size_t bytesToRead = (chan.fmt == I2S_CHANNEL_FMT_RIGHT_LEFT) 
                              ? sizeof(buf) : 640;  // 320 samples * 2 bytes
        
        err = i2s_read(port, buf, bytesToRead, &br, pdMS_TO_TICKS(100));
        if (err != ESP_OK || br == 0) {
            Serial.printf("  Round %d: read error=%d bytes=%d\n", round, err, br);
            continue;
        }

        int numSamples = br / sizeof(int16_t);
        int16_t peak = 0;
        long sum = 0;

        for (int i = 0; i < numSamples; i++) {
            int16_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
            sum += v;
        }

        if (peak > overallPeak) overallPeak = peak;
        overallSum += sum;
        totalSamples += numSamples;

        // Print first 10 raw samples for inspection
        if (round == 0) {
            Serial.printf("  Raw[0..9]: ");
            int show = min(numSamples, 10);
            for (int i = 0; i < show; i++) {
                Serial.printf("%d ", buf[i]);
            }
            Serial.println();
        }
    }

    float avg = totalSamples > 0 ? (float)overallSum / totalSamples : 0;
    Serial.printf("  RESULT: peak=%d  avg=%.1f  samples=%d\n", 
                  overallPeak, avg, totalSamples);

    if (overallPeak > 50) {
        Serial.println("  >>> SIGNAL DETECTED <<<");
    } else if (overallPeak > 5) {
        Serial.println("  (weak signal / noise only)");
    } else {
        Serial.println("  (dead — no signal)");
    }

    // Clean up
    i2s_driver_uninstall(port);
    delay(50);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" INMP441 Microphone Diagnostic");
    Serial.println(" ESP32S3-AI V2.2 — No WiFi, No Speaker, Mic Only");
    Serial.println("=====================================================");
    Serial.printf(" Chip: %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf(" PSRAM: %d bytes\n", ESP.getPsramSize());
    Serial.println();
    Serial.println(" ** Speak loudly near the mic during this test! **");
    Serial.println();

    // Test all combinations
    int testNum = 0;
    for (int p = 0; p < 2; p++) {              // I2S_NUM_0, I2S_NUM_1
        for (int pin = 0; pin < 2; pin++) {      // pin wiring options
            for (int ch = 0; ch < 3; ch++) {     // LEFT, RIGHT, STEREO
                testNum++;
                Serial.printf("=== Test %d/12 ===", testNum);
                testConfig(ports[p], portNames[p],
                          pinConfigs[pin], chanConfigs[ch]);
                delay(100);
            }
        }
    }

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" ALL TESTS COMPLETE");
    Serial.println("=====================================================");
    Serial.println();
    Serial.println("Look for '>>> SIGNAL DETECTED <<<' above.");
    Serial.println("The winning config is the one with the highest peak.");

    digitalWrite(LED_PIN, LOW);
}

void loop() {
    // Blink LED to show test is done
    static unsigned long last = 0;
    if (millis() - last > 1000) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        last = millis();
    }
}
