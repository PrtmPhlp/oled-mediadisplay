#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>
#include <stdarg.h>

#include "credentials.h"

constexpr uint32_t DISPLAY_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5min
// ESP32-C3 I2C pins
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;

constexpr uint16_t MQTT_BUFFER_SIZE = 4096;  // mono bitmap payload is small
constexpr uint16_t LOG_BUFFER_SIZE = 192;
constexpr uint8_t COVER_BYTES_PER_ROW = (48 + 7) / 8;
constexpr uint16_t COVER_BITMAP_BYTES = COVER_BYTES_PER_ROW * 48;

// Layout
constexpr uint8_t TEXT_MARGIN_X = 48 + 4;
constexpr uint8_t ARTIST_Y = 14;
constexpr uint8_t TITLE_Y = 26;
constexpr uint8_t TITLE_Y2 = 36;  // Second Title-Row
constexpr uint8_t TITLE_Y3 = 46;  // Third Title-Row
constexpr uint8_t PAUSED_Y = 56;
constexpr uint8_t X_OFFSET = 2;

// GY-512 (MPU-6050) vibration / knock detection
constexpr uint8_t MPU6050_ADDR = 0x68;
constexpr float KNOCK_THRESHOLD_G = 0.26f;
constexpr uint32_t KNOCK_COOLDOWN_MS = 1000;
constexpr uint32_t MOTION_OFF_DELAY_MS = 10000;  // 5s after last knock -> OFF
constexpr uint16_t CALIBRATION_SAMPLES = 200;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer otaServer(80);  // Web server for ElegantOTA
// 180 degree rotated
U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R2);
// Cover buffers
static uint8_t coverBitmap[(48 * 48) / 8];
static bool coverAvailable = false;
static bool needsRedraw = true;
static bool displaySleeping = false;

// Metadata
static String currentArtist;
static String currentTitle;
static uint32_t lastActivityMs = 0;
static bool playbackActive = false;
static bool coverPending = false;
static uint32_t coverPendingSince = 0;

// MQTT toggle
static bool displayRemoteEnabled = true;

// MPU-6050 motion state
static float baseAx = 0, baseAy = 0, baseAz = 0;
static bool motionSensorAvailable = false;
static bool motionPublished = false;
static bool motionPublishPending = false;
static uint32_t motionLastKnockMs = 0;
static uint32_t motionLastTriggerMs = 0;
static bool motionDiscoverySent = false;

void ensureWifi();
void ensureMqtt();
void refreshDisplay();
void updateDisplayPower(bool wake);
void handleCoverMonoPayload(const uint8_t *payload, size_t length);
void handleArtistPayload(const uint8_t *payload, size_t length);
void handleTitlePayload(const uint8_t *payload, size_t length);
void handlePlaybackEvent(bool started);
void handleDisplayControlPayload(const uint8_t *payload, size_t length);
void publishDisplayState();
String truncateTextToWidth(const String &value, int maxWidth);
void splitTextIntoLines(const String &text, int maxWidth, String &line1, String &line2);
bool isVowel(char c);
void logPayloadPreview(const uint8_t *payload, size_t length);
bool readAccel(int16_t &ax, int16_t &ay, int16_t &az);
bool initMPU6050();
void calibrateBaseline();
void pollMotionSensor();
void publishMotionDiscovery();
void publishMotionState(bool active, bool force = false);
void logLine(const char *message);
void logf(const char *format, ...);

void logLine(const char *message) {
  Serial.println(message);

  if (!mqttClient.connected()) {
    return;
  }

  const String topic = String(MQTT_TOPIC_BASE) + "/debug/log";
  mqttClient.publish(topic.c_str(), message, false);
}

void logf(const char *format, ...) {
  char message[LOG_BUFFER_SIZE];

  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  logLine(message);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  motionSensorAvailable = initMPU6050();
  if (motionSensorAvailable) {
    logLine("MPU-6050 initialized");
    calibrateBaseline();
  } else {
    logLine("MPU-6050 init FAILED - knock detection disabled");
  }

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setPowerSave(0);
  u8g2.enableUTF8Print();
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  // WiFi stability optimizations
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Max TX

  // MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setKeepAlive(15);
  mqttClient.setSocketTimeout(2);
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    logLine("MQTT buffer allocation failed");
  } else {
    logf("MQTT buffer size: %u bytes", MQTT_BUFFER_SIZE);
  }
  mqttClient.setCallback([](char *topic, uint8_t *payload, unsigned int length) {
    const String topicStr(topic);

    if (topicStr.endsWith("/cover_mono")) {
      handleCoverMonoPayload(payload, length);
    } else if (topicStr.endsWith("/artist")) {
      handleArtistPayload(payload, length);
    } else if (topicStr.endsWith("/title")) {
      handleTitlePayload(payload, length);
    } else if (topicStr.endsWith("/play_start") || topicStr.endsWith("/play_resume")) {
      handlePlaybackEvent(true);
    } else if (topicStr.endsWith("/play_end")) {
      handlePlaybackEvent(false);
    } else if (topicStr.endsWith("/display/set")) {
      handleDisplayControlPayload(payload, length);
    }
  });

  ensureWifi();
  ensureMqtt();

  // Publish clean OFF state on boot
  if (mqttClient.connected()) {
    publishMotionState(false, true);
    logLine("Motion: OFF (boot)");
  }

  lastActivityMs = millis();

  // Start ElegantOTA web server
  otaServer.on("/", []() {
    otaServer.send(200, "text/plain", "ESP32-C3 Mediadisplay - OTA: http://" + WiFi.localIP().toString() + "/update");
  });
  ElegantOTA.begin(&otaServer);
  otaServer.begin();
  logf("ElegantOTA ready at http://%s/update", WiFi.localIP().toString().c_str());
}

void loop() {
  pollMotionSensor();

  ensureWifi();
  ensureMqtt();
  mqttClient.loop();
  otaServer.handleClient();
  ElegantOTA.loop();

  const uint32_t now = millis();
  const bool timedOut = (now - lastActivityMs) > DISPLAY_TIMEOUT_MS;
  if (coverPending && (now - coverPendingSince) > 6000) {
    coverPending = false;
    needsRedraw = true;
  }

  const bool shouldBeAwake = displayRemoteEnabled && !timedOut;
  updateDisplayPower(shouldBeAwake);

  if (!timedOut && needsRedraw) {
    refreshDisplay();
    needsRedraw = false;
  }

  delay(10);
}

void ensureWifi() {
  static uint32_t lastWifiAttempt = 0;
  static uint8_t wifiFailCount = 0;
  static bool wifiConnecting = false;
  static bool wasConnected = false;
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      logf("Wi-Fi connected, IP: %s", WiFi.localIP().toString().c_str());
      logf("WiFi RSSI: %d dBm", WiFi.RSSI());
    }
    wifiFailCount = 0;
    wifiConnecting = false;
    wasConnected = true;
    return;
  }
  wasConnected = false;

  if (wifiConnecting) {
    if (now - lastWifiAttempt < 15000) {
      return;
    }
    logLine("Wi-Fi connect timeout");
    wifiConnecting = false;
  } else if (now - lastWifiAttempt < 3000) {
    return;
  }
  lastWifiAttempt = now;

  // After multiple failures, do a full WiFi reset
  if (wifiFailCount >= 5) {
    logLine("WiFi: Too many failures, resetting WiFi stack...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    wifiFailCount = 0;
    wifiConnecting = false;
  }

  logf("Connecting to Wi-Fi %s...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiConnecting = true;
  wifiFailCount++;
  logLine("Wi-Fi connect started");
}

void ensureMqtt() {
  if (mqttClient.connected()) {
    return;
  }

  static uint32_t lastMqttAttempt = 0;
  const uint32_t now = millis();

  if (now - lastMqttAttempt < 5000) {
    return;
  }
  lastMqttAttempt = now;

  if (WiFi.status() != WL_CONNECTED) {
    logLine("MQTT: WiFi not connected, skipping");
    return;
  }

  logLine("Connecting to MQTT...");
  if (mqttClient.connect("esp32c3-cover", MQTT_USER, MQTT_PASS)) {
    logf("MQTT connected, remote logs on %s/debug/log", MQTT_TOPIC_BASE);
    const String base = MQTT_TOPIC_BASE;
    mqttClient.subscribe((base + "/cover_mono").c_str());
    mqttClient.subscribe((base + "/title").c_str());
    mqttClient.subscribe((base + "/artist").c_str());
    mqttClient.subscribe((base + "/track_id").c_str());
    mqttClient.subscribe((base + "/play_start").c_str());
    mqttClient.subscribe((base + "/play_end").c_str());
    mqttClient.subscribe((base + "/play_resume").c_str());
    mqttClient.subscribe((base + "/display/set").c_str());
    publishDisplayState();
    publishMotionDiscovery();
    publishMotionState(motionPublished, true);
  } else {
    logf("MQTT connect failed, rc=%d (retry in 5s)", mqttClient.state());
  }
}

bool readAccel(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(MPU6050_ADDR, (uint8_t)6);
  if (Wire.available() < 6) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  return true;
}

bool initMPU6050() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0) return false;

  // Accel range +-2g (16384 LSB/g)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission(true);

  // DLPF ~44 Hz
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1A);
  Wire.write(0x03);
  Wire.endTransmission(true);

  return true;
}

void calibrateBaseline() {
  Serial.printf("Calibrating baseline (%u samples)...\n", CALIBRATION_SAMPLES);
  float sx = 0, sy = 0, sz = 0;
  uint16_t good = 0;
  for (uint16_t i = 0; i < CALIBRATION_SAMPLES; i++) {
    int16_t ax, ay, az;
    if (readAccel(ax, ay, az)) {
      sx += ax / 16384.0f;
      sy += ay / 16384.0f;
      sz += az / 16384.0f;
      good++;
    }
    delay(5);
  }
  if (good > 0) {
    baseAx = sx / good;
    baseAy = sy / good;
    baseAz = sz / good;
  }
  Serial.printf("Baseline: ax=%.3f ay=%.3f az=%.3f g\n", baseAx, baseAy, baseAz);
}

void publishMotionDiscovery() {
  if (!mqttClient.connected()) return;

  const String discoveryTopic = "homeassistant/binary_sensor/esp32c3_motion/config";
  const String stateTopic = String(MQTT_TOPIC_BASE) + "/motion/state";

  String payload = "{";
  payload += "\"name\":\"ESP32-C3 Motion\",";
  payload += "\"unique_id\":\"esp32c3_motion_mpu6050\",";
  payload += "\"state_topic\":\"" + stateTopic + "\",";
  payload += "\"device_class\":\"vibration\",";
  payload += "\"payload_on\":\"ON\",";
  payload += "\"payload_off\":\"OFF\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"esp32c3_cover\"],";
  payload += "\"name\":\"ESP32-C3 Mediadisplay\",";
  payload += "\"manufacturer\":\"Espressif\",";
  payload += "\"model\":\"ESP32-C3 Super Mini\"";
  payload += "}";
  payload += "}";

  mqttClient.publish(discoveryTopic.c_str(), payload.c_str(), true);
  motionDiscoverySent = true;
  Serial.println("Motion HA discovery published");
}

void publishMotionState(bool active, bool force) {
  const char *state = active ? "ON" : "OFF";

  if (!force && motionPublished == active && !motionPublishPending) {
    return;
  }

  motionPublished = active;
  if (!mqttClient.connected()) {
    motionPublishPending = true;
    return;
  }

  const String topic = String(MQTT_TOPIC_BASE) + "/motion/state";
  mqttClient.publish(topic.c_str(), state, true);
  motionPublishPending = false;
  Serial.printf("Motion: %s\n", state);
}

void pollMotionSensor() {
  if (!motionSensorAvailable) return;

  if (motionPublishPending && mqttClient.connected()) {
    publishMotionState(motionPublished, true);
  }

  int16_t ax, ay, az;
  if (!readAccel(ax, ay, az)) return;

  const float gx = ax / 16384.0f - baseAx;
  const float gy = ay / 16384.0f - baseAy;
  const float gz = az / 16384.0f - baseAz;
  const float delta = sqrtf(gx * gx + gy * gy + gz * gz);

  const uint32_t now = millis();

  // Knock detected
  if (delta >= KNOCK_THRESHOLD_G && (now - motionLastKnockMs) >= KNOCK_COOLDOWN_MS) {
    motionLastKnockMs = now;
    motionLastTriggerMs = now;
    Serial.printf("Knock detected (delta=%.3fg)\n", delta);

    if (motionPublished && mqttClient.connected()) {
      publishMotionState(false, true);
    }
    publishMotionState(true, true);
  }

  // Auto-OFF after delay
  if (motionPublished && (now - motionLastTriggerMs) >= MOTION_OFF_DELAY_MS) {
    publishMotionState(false, true);
  }
}

void handleCoverMonoPayload(const uint8_t *payload, size_t length) {
  if (length == 2 && payload[0] == '-' && payload[1] == '-') {
    return;
  }
  if (length != COVER_BITMAP_BYTES) {
    Serial.printf("Cover mono wrong size: %u bytes\n", static_cast<unsigned>(length));
    return;
  }

  memcpy(coverBitmap, payload, COVER_BITMAP_BYTES);
  coverAvailable = true;
  coverPending = false;
  playbackActive = true;
  needsRedraw = true;
  lastActivityMs = millis();
  Serial.printf("Cover mono received (%u bytes)\n", COVER_BITMAP_BYTES);
}

void handleArtistPayload(const uint8_t *payload, size_t length) {
  String newArtist((const char *)payload, length);
  newArtist.trim();
  if (newArtist != currentArtist) {
    currentArtist = newArtist;
    needsRedraw = true;
    Serial.printf("Artist: %s\n", currentArtist.c_str());
  }
  lastActivityMs = millis();
  playbackActive = true;
}

void handleTitlePayload(const uint8_t *payload, size_t length) {
  String newTitle((const char *)payload, length);
  newTitle.trim();
  if (newTitle != currentTitle) {
    currentTitle = newTitle;
    needsRedraw = true;
    Serial.printf("Title: %s\n", currentTitle.c_str());
  }
  lastActivityMs = millis();
  playbackActive = true;
}

void handlePlaybackEvent(bool started) {
  playbackActive = started;
  if (started) {
    lastActivityMs = millis();
    Serial.println("Playback started");
  } else {
    Serial.println("Playback ended");
  }
}

void handleDisplayControlPayload(const uint8_t *payload, size_t length) {
  String cmd((const char *)payload, length);
  cmd.trim();
  cmd.toUpperCase();

  bool newState = displayRemoteEnabled;
  if (cmd == "ON" || cmd == "1" || cmd == "TRUE") {
    newState = true;
  } else if (cmd == "OFF" || cmd == "0" || cmd == "FALSE") {
    newState = false;
  }

  if (newState != displayRemoteEnabled) {
    displayRemoteEnabled = newState;
    Serial.printf("MQTT Set: %s\n", displayRemoteEnabled ? "ON" : "OFF");
    if (displayRemoteEnabled) {
      lastActivityMs = millis();
      needsRedraw = true;
    }

    publishDisplayState();
  }
}

void publishDisplayState() {
  if (!mqttClient.connected()) {
    return;
  }
  const String stateTopic = String(MQTT_TOPIC_BASE) + "/display/state";
  const char *stateStr = displayRemoteEnabled ? "ON" : "OFF";
  mqttClient.publish(stateTopic.c_str(), stateStr, true);  // retained
  Serial.printf("Published display state: %s\n", stateStr);
}

void refreshDisplay() {
  u8g2.clearBuffer();

  const int coverY = (64 - 48) / 2;
  if (coverAvailable) {
    u8g2.drawXBMP(X_OFFSET, coverY, 48, 48, coverBitmap);
  }
  const int textMaxWidth = 128 - TEXT_MARGIN_X - 4 - X_OFFSET;

  // Artist
  u8g2.setFont(u8g2_font_4x6_tf);
  if (currentArtist.length()) {
    const String text = truncateTextToWidth(currentArtist, textMaxWidth);
    u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, ARTIST_Y, text.c_str());
  }

  // Title
  u8g2.setFont(u8g2_font_6x10_tf);
  if (currentTitle.length()) {
    const int titleWidth = u8g2.getUTF8Width(currentTitle.c_str());
    if (titleWidth <= textMaxWidth) {
      u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, TITLE_Y, currentTitle.c_str());
    } else {
      String line1, line2;
      splitTextIntoLines(currentTitle, textMaxWidth, line1, line2);
      u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, TITLE_Y, line1.c_str());
      const int line2Width = u8g2.getUTF8Width(line2.c_str());
      if (line2Width <= textMaxWidth) {
        u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, TITLE_Y2, line2.c_str());
      } else {
        String line2a, line2b;
        splitTextIntoLines(line2, textMaxWidth, line2a, line2b);
        u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, TITLE_Y2, line2a.c_str());
        u8g2.drawUTF8(TEXT_MARGIN_X + X_OFFSET, TITLE_Y3, line2b.c_str());
      }
    }
  }

  // Paused
  if (!playbackActive) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(TEXT_MARGIN_X + X_OFFSET, PAUSED_Y, "Paused");
  }

  // Play symbol
  if (playbackActive) {
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawTriangle(103 + X_OFFSET, 57, 103 + X_OFFSET, 63, 106 + X_OFFSET, 60);
    u8g2.drawStr(108 + X_OFFSET, 63, "Play");
  }

  u8g2.sendBuffer();
}

void updateDisplayPower(bool wake) {
  if (wake && displaySleeping) {
    u8g2.setPowerSave(0);
    displaySleeping = false;
    needsRedraw = true;
    Serial.println("Display wake");
  } else if (!wake && !displaySleeping) {
    u8g2.setPowerSave(1);
    displaySleeping = true;
    Serial.println("Display sleep");
  }
}

String truncateTextToWidth(const String &value, int maxWidth) {
  if (value.length() == 0 || maxWidth <= 0) return String();
  if (u8g2.getUTF8Width(value.c_str()) <= maxWidth) return value;
  for (int end = value.length(); end > 0; end--) {
    if (u8g2.getUTF8Width(value.substring(0, end).c_str()) <= maxWidth)
      return value.substring(0, end);
  }
  return String();
}

bool isVowel(char c) {
  c = tolower(c);
  return strchr("aeiouäöü", c) != nullptr;
}

void splitTextIntoLines(const String &text, int maxWidth, String &line1, String &line2) {
  line1 = line2 = "";
  int lastSpace = -1;
  String current = "";

  for (int i = 0; i < text.length(); i++) {
    current += text.charAt(i);
    if (text.charAt(i) == ' ') lastSpace = i;

    if (u8g2.getUTF8Width(current.c_str()) > maxWidth) {
      if (lastSpace > 0) {
        line1 = text.substring(0, lastSpace);
        line2 = text.substring(lastSpace + 1);
      } else {
        for (int j = i - 1; j > 0; j--) {
          if (isVowel(text.charAt(j))) {
            line1 = text.substring(0, j + 1) + "-";
            line2 = text.substring(j + 1);
            break;
          }
        }
        if (line1.length() == 0) {
          line1 = text.substring(0, i);
          line2 = text.substring(i);
        }
      }
      break;
    }
  }

  if (line1.length() == 0) {
    line1 = text;
    line2 = "";
  }
  line1 = truncateTextToWidth(line1, maxWidth);
}
