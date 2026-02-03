#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <Wire.h>

// ---------- Configuration ----------
// Credentials are stored in a separate file (not in version control)
// Copy credentials.example.h to credentials.h and fill in your values
#include "credentials.h"

// MQTT topic base (can be overridden in credentials.h)
#ifndef MQTT_TOPIC_BASE
constexpr const char *MQTT_TOPIC_BASE = "iotstack/shairport-extension";
#endif

// Display + image settings
constexpr uint8_t COVER_SIZE = 48;                            // must be 32, 48, or 64 to match SSD1309 height comfortably
constexpr uint32_t DISPLAY_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 minutes
constexpr uint8_t BRIGHTNESS = 255;                           // 0-255; maps to contrast command

// ESP32-C3 I2C pins (adjust to your wiring)
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;

// MQTT / bitmap limits
constexpr uint16_t MQTT_BUFFER_SIZE = 4096;  // mono bitmap payload is small
constexpr uint8_t COVER_BYTES_PER_ROW = (COVER_SIZE + 7) / 8;
constexpr uint16_t COVER_BITMAP_BYTES = COVER_BYTES_PER_ROW * COVER_SIZE;

// Font + layout
constexpr uint8_t TEXT_MARGIN_X = COVER_SIZE + 4;
constexpr uint8_t ARTIST_Y = 14;
constexpr uint8_t TITLE_Y = 26;
constexpr uint8_t TITLE_Y2 = 36;  // Zweite Zeile für Title
constexpr uint8_t TITLE_Y3 = 46;  // Dritte Zeile für Title
constexpr uint8_t PAUSED_Y = 56;
constexpr uint16_t COVER_PENDING_TIMEOUT_MS = 6000;
constexpr uint8_t DITHER_THRESHOLD = 128;
constexpr bool INVERT_COVER = false;
constexpr bool CONTRAST_STRETCH = false;
constexpr bool FIT_FULL_COVER = false;

// ---------- Globals ----------
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
// U8G2_R0 = no rotation
U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0);
// Cover buffers
static uint8_t coverBitmap[(COVER_SIZE * COVER_SIZE) / 8];
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

// Forward declarations
void ensureWifi();
void ensureMqtt();
void refreshDisplay();
void updateDisplayPower(bool wake);
void handleCoverMonoPayload(const uint8_t *payload, size_t length);
void handleArtistPayload(const uint8_t *payload, size_t length);
void handleTitlePayload(const uint8_t *payload, size_t length);
void handlePlaybackEvent(bool started);
String truncateTextToWidth(const String &value, int maxWidth);
void splitTextIntoLines(const String &text, int maxWidth, String &line1, String &line2);
bool isVowel(char c);
void logPayloadPreview(const uint8_t *payload, size_t length);

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  u8g2.begin();
  u8g2.setContrast(BRIGHTNESS);
  u8g2.setPowerSave(0);
  u8g2.enableUTF8Print();
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  WiFi.mode(WIFI_STA);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.println("MQTT buffer allocation failed");
  } else {
    Serial.printf("MQTT buffer size: %u bytes\n", MQTT_BUFFER_SIZE);
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
    }
  });

  ensureWifi();
  ensureMqtt();
  lastActivityMs = millis();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  mqttClient.loop();

  const uint32_t now = millis();
  const bool timedOut = (now - lastActivityMs) > DISPLAY_TIMEOUT_MS;
  if (coverPending && (now - coverPendingSince) > COVER_PENDING_TIMEOUT_MS) {
    coverPending = false;
    needsRedraw = true;
  }
  updateDisplayPower(!timedOut);

  if (!timedOut && needsRedraw) {
    refreshDisplay();
    needsRedraw = false;
  }

  delay(10);
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Connecting to Wi-Fi %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 60) {
    delay(500);
    Serial.print('.');
    retries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connect timeout");
  }
}

void ensureMqtt() {
  if (mqttClient.connected()) {
    return;
  }

  Serial.println("Connecting to MQTT...");
  while (!mqttClient.connected()) {
    if (mqttClient.connect("esp32c3-cover", MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT connected");
      const String base = MQTT_TOPIC_BASE;
      mqttClient.subscribe((base + "/cover_mono").c_str());
      mqttClient.subscribe((base + "/title").c_str());
      mqttClient.subscribe((base + "/artist").c_str());
      mqttClient.subscribe((base + "/track_id").c_str());
      mqttClient.subscribe((base + "/play_start").c_str());
      mqttClient.subscribe((base + "/play_end").c_str());
      mqttClient.subscribe((base + "/play_resume").c_str());
      break;
    } else {
      Serial.printf("MQTT connect failed, rc=%d\n", mqttClient.state());
      delay(2000);
    }
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

void refreshDisplay() {
  u8g2.clearBuffer();

  const int coverY = (64 - COVER_SIZE) / 2;
  if (coverAvailable) {
    u8g2.drawXBMP(0, coverY, COVER_SIZE, COVER_SIZE, coverBitmap);
  } else if (coverPending) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(14, 36, "...");
  } else {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(6, 36, "Wait");
  }

  // Verfügbare Breite für Text (Display 128px - Cover - Margin)
  const int textMaxWidth = 128 - TEXT_MARGIN_X - 4;

  // Artist: kleine Schrift
  u8g2.setFont(u8g2_font_4x6_tf);
  if (currentArtist.length()) {
    const String text = truncateTextToWidth(currentArtist, textMaxWidth);
    u8g2.drawUTF8(TEXT_MARGIN_X, ARTIST_Y, text.c_str());
  }

  // Title: mittlere Schrift (mit intelligentem Wrap auf bis zu 3 Zeilen)
  u8g2.setFont(u8g2_font_6x10_tf);
  if (currentTitle.length()) {
    const int titleWidth = u8g2.getUTF8Width(currentTitle.c_str());

    if (titleWidth <= textMaxWidth) {
      // Passt in eine Zeile
      u8g2.drawUTF8(TEXT_MARGIN_X, TITLE_Y, currentTitle.c_str());
    } else {
      // Auf mehrere Zeilen aufteilen (intelligent)
      String line1, line2;
      splitTextIntoLines(currentTitle, textMaxWidth, line1, line2);

      u8g2.drawUTF8(TEXT_MARGIN_X, TITLE_Y, line1.c_str());

      // Prüfe ob Zeile 2 auch noch auf 2 Zeilen aufgeteilt werden muss
      const int line2Width = u8g2.getUTF8Width(line2.c_str());
      if (line2Width <= textMaxWidth) {
        u8g2.drawUTF8(TEXT_MARGIN_X, TITLE_Y2, line2.c_str());
      } else {
        // Zeile 2 nochmal aufteilen für 3 Zeilen gesamt
        String line2a, line2b;
        splitTextIntoLines(line2, textMaxWidth, line2a, line2b);
        u8g2.drawUTF8(TEXT_MARGIN_X, TITLE_Y2, line2a.c_str());
        u8g2.drawUTF8(TEXT_MARGIN_X, TITLE_Y3, line2b.c_str());
      }
    }
  }

  // Paused: kleine Schrift
  if (!playbackActive) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(TEXT_MARGIN_X, PAUSED_Y, "Paused");
  }

  // Play-Indikator unten rechts (wenn aktiv)
  if (playbackActive) {
    u8g2.setFont(u8g2_font_4x6_tf);
    // Play-Symbol (Dreieck) + "Play" Text
    // Dreieck: 3 Punkte bei x=103, y=58-62
    u8g2.drawTriangle(103, 57, 103, 63, 106, 60);
    u8g2.drawStr(108, 63, "Play");
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
  if (value.length() == 0 || maxWidth <= 0) {
    return String();
  }
  if (u8g2.getUTF8Width(value.c_str()) <= maxWidth) {
    return value;
  }
  int end = value.length();
  while (end > 0) {
    const String candidate = value.substring(0, end);
    if (u8g2.getUTF8Width(candidate.c_str()) <= maxWidth) {
      return candidate;
    }
    end--;
  }
  return String();
}

bool isVowel(char c) {
  c = tolower(c);
  return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'ä' || c == 'ö' || c == 'ü');
}

void splitTextIntoLines(const String &text, int maxWidth, String &line1, String &line2) {
  // Strategie: Zeilenumbruch wie in Textverarbeitungsprogrammen
  // 1. An Leerzeichen trennen (bevorzugt)
  // 2. An Bindestrichen trennen
  // 3. Wort mit Silbentrennung (nach Vokal + "-")

  line1 = "";
  line2 = "";

  // Versuche Wort für Wort zu füllen
  int lastSpace = -1;
  String currentLine = "";

  for (int i = 0; i < text.length(); i++) {
    currentLine += text.charAt(i);

    if (text.charAt(i) == ' ') {
      lastSpace = i;
    }

    // Prüfe ob die aktuelle Zeile zu lang wird
    if (u8g2.getUTF8Width(currentLine.c_str()) > maxWidth) {
      if (lastSpace > 0) {
        // Trenne am letzten Leerzeichen
        line1 = text.substring(0, lastSpace);
        line2 = text.substring(lastSpace + 1);
        break;
      } else {
        // Kein Leerzeichen gefunden - versuche Silbentrennung
        // Gehe rückwärts und finde Vokal für Trennung
        for (int j = i - 1; j > 0; j--) {
          if (isVowel(text.charAt(j))) {
            line1 = text.substring(0, j + 1) + "-";
            line2 = text.substring(j + 1);
            break;
          }
        }

        // Fallback: hart trennen wenn kein Vokal gefunden
        if (line1.length() == 0) {
          line1 = text.substring(0, i);
          line2 = text.substring(i);
        }
        break;
      }
    }
  }

  // Falls Text komplett in erste Zeile passt (sollte nicht passieren)
  if (line1.length() == 0) {
    line1 = text;
    line2 = "";
  }

  // Nur Zeile 1 kürzen - Zeile 2 bleibt für weitere Aufteilung erhalten
  line1 = truncateTextToWidth(line1, maxWidth);
  // line2 wird NICHT gekürzt, damit sie ggf. nochmals aufgeteilt werden kann
}
