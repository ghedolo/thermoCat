#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <driver/i2s.h>
#include <cmath>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "freq_compensation.h"
#include "meow_sample.h"
#include "purr_sample.h"
#include "hiss_sample.h"
#include "mqttCred.h"

// ── I2S pins (MAX98357A) ──
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  27

// ── I2S config ──
#define SAMPLE_RATE   22050
#define I2S_PORT      I2S_NUM_0
#define DMA_BUF_COUNT 4
#define DMA_BUF_LEN   256

// ── Temperature -> behavior ──
//  < 20C  -> purr   — relaxed cat
// 20–30C  -> meow   — cat asking for attention
//  > 30C  -> hiss   — irritated cat
// Smooth transitions with crossfade

Adafruit_MLX90614 mlx;

// ── Volume (0.0 – 1.0) — low default ──
volatile float master_volume = 0.15f;

// ── WiFi NVS + MQTT ──
Preferences prefs;
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
unsigned long lastMqttReconnect = 0;
unsigned long lastTempPublish = 0;
float lastPublishedObj = -999;
float lastPublishedAmb = -999;

String serialReadLine() {
    String line = "";
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) return line;
            } else {
                line += c;
                Serial.print(c);
            }
        }
        delay(10);
    }
}

void wifiAskCredentials() {
    Serial.println("\n== WiFi Configuration ==");
    Serial.print("SSID: ");
    String ssid = serialReadLine();
    Serial.println();
    Serial.print("Password: ");
    String pass = serialReadLine();
    Serial.println();

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    Serial.println("Credentials saved to NVS.");
}

bool wifiConnect() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() == 0) return false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("WiFi: %s ", ssid.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        delay(250);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf(" OK (%s)\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println(" FAIL");
    return false;
}

void wifiSetup() {
    while (!wifiConnect()) {
        Serial.println("WiFi not connected. Missing or invalid credentials.");
        wifiAskCredentials();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicStr(topic);
    String volTopic = String(START_TOPIC) + "/cat/cmd/volume";
    if (topicStr == volTopic) {
        char buf[8];
        int len = (length < 7) ? length : 7;
        memcpy(buf, payload, len);
        buf[len] = 0;
        int vol = atoi(buf);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        master_volume = vol / 100.0f;
        Serial.printf("MQTT volume: %d%%\n", vol);
    }
}

void mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (mqtt.connected()) return;

    unsigned long now = millis();
    if (now - lastMqttReconnect < 5000) return;
    lastMqttReconnect = now;

    Serial.print("MQTT connecting...");
    String clientId = "thermocat-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println(" OK");

        // Publish system info on separate retained topics
        String base = String(START_TOPIC) + "/cat/system/";
        mqtt.publish((base + "name").c_str(), "thermoCat", true);
        mqtt.publish((base + "compiled").c_str(), __DATE__ " " __TIME__, true);

        // Subscribe to volume command
        String volTopic = String(START_TOPIC) + "/cat/cmd/volume";
        mqtt.subscribe(volTopic.c_str());
        Serial.printf("Sub: %s\n", volTopic.c_str());
    } else {
        Serial.printf(" FAIL rc=%d\n", mqtt.state());
    }
}

// ── Synthesis state ──
float phase_meow      = 0;
float meow_time       = 0;

float current_temp  = 25.0f;
float smoothed_temp = 25.0f;

// ── Debug: column counter for line wrap ──
// Sounds: \ purr, | meow, / hiss
// Index: 3 binary bits, _ = 0, - = 1
volatile int debug_col = 0;
void debugSound(char tag, int idx) {
    char buf[5] = {
        tag,
        (char)((idx & 4) ? '-' : '_'),
        (char)((idx & 2) ? '-' : '_'),
        (char)((idx & 1) ? '-' : '_'),
        0
    };
    Serial.print(buf);
    debug_col += 4;
    if (debug_col >= 80) {
        Serial.println();
        debug_col = 0;
    }
}

// ── Random — three separate generators ──
uint32_t noise_seed = 12345;   // for audio noise (hiss)
uint32_t meow_rng = 67890;     // for meow choices (sample/pause)
uint32_t purr_rng = 54321;     // for purr choices (sample/pause)

uint32_t xorshift32(uint32_t &seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

float whiteNoise() {
    return (float)(int32_t)xorshift32(noise_seed) / (float)INT32_MAX;
}


// ── PURR from real sample with crossfade and pauses ──
int purr_cur_idx = 0;
int purr_cur_pos = 0;
int purr_next_idx = -1;
int purr_next_pos = 0;
int purr_pause_counter = 0;
bool purr_in_pause = false;
bool purr_logged = false;

#define PURR_XFADE_LEN (int)(0.3f * SAMPLE_RATE)  // 300ms crossfade

void purrPickNext() {
    purr_rng ^= (uint32_t)esp_timer_get_time();
    purr_next_idx = xorshift32(purr_rng) % NUM_PURR_SAMPLES;
    if (purr_next_idx == purr_cur_idx)
        purr_next_idx = (purr_next_idx + 1) % NUM_PURR_SAMPLES;
    purr_next_pos = 0;
}

void purrSchedulePause() {
    purr_rng ^= (uint32_t)esp_timer_get_time();
    int min_p = 1 * SAMPLE_RATE;
    int max_p = 3 * SAMPLE_RATE;
    purr_pause_counter = min_p + (xorshift32(purr_rng) % (max_p - min_p));
    purr_in_pause = true;
    purr_logged = false;
}

float synthPurr(float intensity) {
    if (purr_in_pause) {
        purr_pause_counter--;
        if (purr_pause_counter <= 0) {
            purr_in_pause = false;
            purr_rng ^= (uint32_t)esp_timer_get_time();
            purr_cur_idx = xorshift32(purr_rng) % NUM_PURR_SAMPLES;
            purr_cur_pos = 0;
            purr_next_idx = -1;
            purr_logged = false;
        }
        return 0;
    }

    if (!purr_logged) {
        debugSound('\\', purr_cur_idx);
        purr_logged = true;
    }

    int cur_len = PURR_LENGTHS[purr_cur_idx];
    int remaining = cur_len - purr_cur_pos;

    if (remaining <= PURR_XFADE_LEN && purr_next_idx < 0) {
        purr_rng ^= (uint32_t)esp_timer_get_time();
        if ((xorshift32(purr_rng) % 100) < 30) {
            float fade = (float)remaining / PURR_XFADE_LEN;
            float s = (float)PURR_SAMPLES[purr_cur_idx][purr_cur_pos] / 127.0f;
            purr_cur_pos++;
            if (purr_cur_pos >= cur_len) {
                purrSchedulePause();
            }
            return s * fade * intensity * 0.6f;
        }
        purrPickNext();
    }

    if (purr_next_idx >= 0 && remaining <= PURR_XFADE_LEN) {
        float t = 1.0f - (float)remaining / PURR_XFADE_LEN;
        float s_cur = (float)PURR_SAMPLES[purr_cur_idx][purr_cur_pos] / 127.0f;
        float s_next = 0;
        int next_len = PURR_LENGTHS[purr_next_idx];
        if (purr_next_pos < next_len) {
            s_next = (float)PURR_SAMPLES[purr_next_idx][purr_next_pos] / 127.0f;
            purr_next_pos++;
        }
        purr_cur_pos++;

        if (purr_cur_pos >= cur_len) {
            purr_cur_idx = purr_next_idx;
            purr_cur_pos = purr_next_pos;
            purr_next_idx = -1;
            purr_logged = false;
        }

        float mixed = s_cur * (1.0f - t) + s_next * t;
        return mixed * intensity * 0.6f;
    }

    float s = (float)PURR_SAMPLES[purr_cur_idx][purr_cur_pos] / 127.0f;
    purr_cur_pos++;

    if (purr_cur_pos >= cur_len) {
        purr_rng ^= (uint32_t)esp_timer_get_time();
        purr_cur_idx = xorshift32(purr_rng) % NUM_PURR_SAMPLES;
        purr_cur_pos = 0;
        purr_logged = false;
    }

    return s * intensity * 0.6f;
}

// ── MEOW from real sample ──
int meow_sample_idx = 0;      // which sample is playing
int meow_play_pos = -1;       // position in sample (-1 = pause)
int meow_pause_counter = 0;   // remaining pause samples
int meow_next_pause = 0;      // pause before next meow

void meowStartNext() {
    meow_rng ^= (uint32_t)esp_timer_get_time();
    meow_sample_idx = xorshift32(meow_rng) % NUM_MEOW_SAMPLES;
    meow_play_pos = 0;
    debugSound('|', meow_sample_idx);
}

void meowSchedulePause() {
    // Random pause between 4 and 10 seconds (~4-8 meows/minute)
    meow_rng ^= (uint32_t)esp_timer_get_time();
    int min_pause = 4 * SAMPLE_RATE;
    int max_pause = 10 * SAMPLE_RATE;
    meow_pause_counter = min_pause + (xorshift32(meow_rng) % (max_pause - min_pause));
    meow_play_pos = -1;
}

float synthMeow(float intensity) {
    if (meow_play_pos < 0) {
        // Paused
        meow_pause_counter--;
        if (meow_pause_counter <= 0) {
            meowStartNext();
        }
        return 0;
    }

    // Play sample
    int len = MEOW_LENGTHS[meow_sample_idx];
    if (meow_play_pos >= len) {
        meowSchedulePause();
        return 0;
    }

    float sample = (float)MEOW_SAMPLES[meow_sample_idx][meow_play_pos] / 127.0f;
    meow_play_pos++;

    return sample * intensity * 0.5f;
}

// ── HISS from real sample ──
int hiss_sample_idx = 0;
int hiss_play_pos = -1;
int hiss_pause_counter = 0;
uint32_t hiss_rng = 33333;

void hissStartNext() {
    hiss_rng ^= (uint32_t)esp_timer_get_time();
    hiss_sample_idx = xorshift32(hiss_rng) % NUM_HISS_SAMPLES;
    hiss_play_pos = 0;
    debugSound('/', hiss_sample_idx);
}

void hissSchedulePause() {
    hiss_rng ^= (uint32_t)esp_timer_get_time();
    int min_p = 2 * SAMPLE_RATE;
    int max_p = 5 * SAMPLE_RATE;
    hiss_pause_counter = min_p + (xorshift32(hiss_rng) % (max_p - min_p));
    hiss_play_pos = -1;
}

float synthHiss(float intensity) {
    if (hiss_play_pos < 0) {
        hiss_pause_counter--;
        if (hiss_pause_counter <= 0) {
            hissStartNext();
        }
        return 0;
    }

    int len = HISS_LENGTHS[hiss_sample_idx];
    if (hiss_play_pos >= len) {
        hissSchedulePause();
        return 0;
    }

    float sample = (float)HISS_SAMPLES[hiss_sample_idx][hiss_play_pos] / 127.0f;
    hiss_play_pos++;

    return sample * intensity * 0.6f;
}

// ── I2S setup ──
void setupI2S() {
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = DMA_BUF_COUNT;
    config.dma_buf_len = DMA_BUF_LEN;
    config.use_apll = false;

    i2s_driver_install(I2S_PORT, &config, 0, NULL);

    i2s_pin_config_t pins = {};
    pins.bck_io_num = I2S_BCLK;
    pins.ws_io_num = I2S_LRC;
    pins.data_out_num = I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ── Audio buffer ──
int16_t audio_buf[DMA_BUF_LEN * 2];  // stereo

void fillBuffer() {

    // Temperature zones:
    //  < 20C  -> purr
    // 20-24C  -> crossfade purr -> meow
    // 24-34C  -> meow (wide zone)
    // 34-38C  -> crossfade meow -> hiss
    //  > 38C  -> hiss
    float purr_int = 0, meow_int = 0, hiss_int = 0;

    if (smoothed_temp < 20.0f) {
        purr_int = 1.0f;
    } else if (smoothed_temp < 24.0f) {
        float t = (smoothed_temp - 20.0f) / 4.0f;
        purr_int = 1.0f - t;
        meow_int = t;
    } else if (smoothed_temp < 34.0f) {
        meow_int = 1.0f;
    } else if (smoothed_temp < 38.0f) {
        float t = (smoothed_temp - 34.0f) / 4.0f;
        meow_int = 1.0f - t;
        hiss_int = t;
    } else {
        hiss_int = 1.0f;
    }

    for (int i = 0; i < DMA_BUF_LEN; i++) {
        float sample = 0;

        if (purr_int > 0.01f) sample += synthPurr(purr_int);
        if (meow_int > 0.01f) sample += synthMeow(meow_int);
        if (hiss_int > 0.01f) sample += synthHiss(hiss_int);

        // Master volume + soft clip (low gain to avoid distortion)
        sample *= master_volume;
        if (sample > 0.9f) sample = 0.9f;
        else if (sample < -0.9f) sample = -0.9f;

        int16_t s = (int16_t)(sample * 16000.0f);
        audio_buf[i * 2]     = s;  // L
        audio_buf[i * 2 + 1] = s;  // R
    }
}

// ── Audio task (core 1) ──
void audioTask(void *param) {
    while (true) {
        fillBuffer();
        size_t written;
        i2s_write(I2S_PORT, audio_buf, sizeof(audio_buf), &written, portMAX_DELAY);
    }
}

// ── Setup ──
void setup() {
    Serial.begin(115200);
    Serial.println("thermoCat - starting...");

    // ── WiFi from NVS (asks over serial if missing/failing) ──
    wifiSetup();

    // ── MQTT TLS ──
    espClient.setInsecure();
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);

    Wire.begin(21, 22);

    if (!mlx.begin()) {
        Serial.println("MLX90614 not found!");
        while (1) delay(100);
    }
    Serial.println("MLX90614 OK");

    current_temp = mlx.readObjectTempC();
    smoothed_temp = current_temp;
    Serial.printf("Initial temp: %.1fC\n", current_temp);

    meowSchedulePause();  // start with random pause
    hissSchedulePause();  // start with random pause
    setupI2S();

    // Audio task on core 1
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 10, NULL, 1);

    Serial.println("");
    Serial.println("=============================");
    Serial.println("  thermoCat v1.0");
    Serial.println("  ghedo 2026");
    Serial.printf( "  Build: %s %s\n", __DATE__, __TIME__);
    Serial.println("=============================");
    Serial.println("");
    Serial.println("Serial commands:");
    Serial.println("  >  Volume +5%");
    Serial.println("  <  Volume -5%");
    Serial.printf( "  Current volume: %.0f%%\n", master_volume * 100.0f);
    Serial.println("");
    Serial.println("MLX90614 thermometer (object):");
    Serial.println("  < 20C      PURR");
    Serial.println("  20-24C     PURR + MEOW");
    Serial.println("  24-34C     MEOW");
    Serial.println("  34-38C     MEOW + HISS");
    Serial.println("  > 38C      HISS");
    Serial.println("");
    Serial.printf( "Initial temp: %.1fC\n", current_temp);
    Serial.println("=============================");
    Serial.println("");

    Serial.println("Sound debug: \\ purr, | meow, / hiss");
    Serial.println("Index: 3 bits (_ = 0, - = 1)");
    Serial.println();

    // First MQTT connection
    mqttConnect();
}

// ── Loop: temperature reading + serial volume control ──
void loop() {
    // ── MQTT ──
    if (!mqtt.connected()) mqttConnect();
    mqtt.loop();

    // Volume control over serial
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '>') {
            master_volume += 0.05f;
            if (master_volume > 1.0f) master_volume = 1.0f;
            Serial.print(master_volume >= 1.0f ? "=" : ">");
        } else if (c == '<') {
            master_volume -= 0.05f;
            if (master_volume < 0.0f) master_volume = 0.0f;
            Serial.print(master_volume <= 0.0f ? "=" : "<");
        }
    }

    float raw_temp = mlx.readObjectTempC();
    float ambient_temp = mlx.readAmbientTempC();
    // Discard bad readings (NaN or out of sensor range)
    if (!isnan(raw_temp) && raw_temp > -40.0f && raw_temp < 125.0f) {
        current_temp = raw_temp;
    }
    smoothed_temp += 0.3f * (current_temp - smoothed_temp);

    // Determine current register
    const char *state;
    if (smoothed_temp < 20.0f)
        state = "PURR";
    else if (smoothed_temp < 24.0f)
        state = "PURR+MEOW";
    else if (smoothed_temp < 34.0f)
        state = "MEOW";
    else if (smoothed_temp < 38.0f)
        state = "MEOW+HISS";
    else
        state = "HISS";

    (void)state;

    // ── Publish temperatures over MQTT ──
    if (mqtt.connected()) {
        unsigned long now = millis();
        bool changed = (fabsf(current_temp - lastPublishedObj) >= 0.5f) ||
                       (fabsf(ambient_temp - lastPublishedAmb) >= 0.5f);
        bool timeout = (now - lastTempPublish >= 60000);

        if (changed || timeout) {
            String base = String(START_TOPIC) + "/cat/system/";
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", current_temp);
            mqtt.publish((base + "Tir").c_str(), buf, true);
            snprintf(buf, sizeof(buf), "%.1f", ambient_temp);
            mqtt.publish((base + "Tambient").c_str(), buf, true);
            lastPublishedObj = current_temp;
            lastPublishedAmb = ambient_temp;
            lastTempPublish = now;
        }
    }

    delay(500);
}
