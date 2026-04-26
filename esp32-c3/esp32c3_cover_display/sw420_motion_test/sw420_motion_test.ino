#include <Arduino.h>

constexpr int SW420_DO_PIN = 4;
constexpr int ACTIVE_STATE = LOW;
constexpr uint32_t SERIAL_WAIT_MS = 4000;
constexpr uint32_t POLL_INTERVAL_MS = 10;
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t STATUS_INTERVAL_MS = 1000;

static int stableState = HIGH;
static int lastRawState = HIGH;
static uint32_t lastRawChangeMs = 0;
static uint32_t lastPollMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t triggerCount = 0;

const char *levelText(int state) {
  return state == HIGH ? "HIGH" : "LOW";
}

bool isTriggered(int state) {
  return state == ACTIVE_STATE;
}

void printStateChange(int state) {
  Serial.print("Signal -> ");
  Serial.print(levelText(state));
  Serial.print(" (");
  Serial.print(isTriggered(state) ? "triggered" : "idle");
  Serial.println(")");
}

void setup() {
  Serial.begin(115200);
  const uint32_t serialStartMs = millis();
  while (!Serial && (millis() - serialStartMs) < SERIAL_WAIT_MS) {
    delay(10);
  }
  delay(200);

  pinMode(SW420_DO_PIN, INPUT_PULLUP);

  stableState = digitalRead(SW420_DO_PIN);
  lastRawState = stableState;
  lastRawChangeMs = millis();
  lastStatusMs = lastRawChangeMs;

  Serial.println();
  Serial.println("Boot OK");
  Serial.println("SW-420 motion sensor test");
  Serial.printf("DO pin: GPIO%d\n", SW420_DO_PIN);
  Serial.printf("Active state: %s\n", levelText(ACTIVE_STATE));
  Serial.println("Wiring:");
  Serial.println("  VCC -> 3V3");
  Serial.println("  GND -> GND");
  Serial.println("  DO  -> GPIO4");
  Serial.println("Turn the potentiometer slowly to adjust sensitivity.");
  Serial.println("If your board triggers on HIGH instead, set ACTIVE_STATE to HIGH.");
  Serial.println("If nothing changes at all, test another free GPIO such as GPIO3.");
  printStateChange(stableState);
}

void loop() {
  const uint32_t now = millis();
  if (now - lastPollMs < POLL_INTERVAL_MS) {
    return;
  }
  lastPollMs = now;

  const int rawState = digitalRead(SW420_DO_PIN);

  if (rawState != lastRawState) {
    lastRawState = rawState;
    lastRawChangeMs = now;
  }

  if ((now - lastRawChangeMs) >= DEBOUNCE_MS && stableState != lastRawState) {
    stableState = lastRawState;
    if (isTriggered(stableState)) {
      triggerCount++;
      Serial.printf("Trigger event #%lu\n", static_cast<unsigned long>(triggerCount));
    }
    printStateChange(stableState);
  }

  if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    Serial.printf(
        "Status: raw=%s stable=%s triggers=%lu\n",
        levelText(rawState),
        levelText(stableState),
        static_cast<unsigned long>(triggerCount));
  }
}