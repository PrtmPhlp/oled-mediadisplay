#include <Arduino.h>
#include <Wire.h>
#include <math.h>

constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;
constexpr uint8_t MPU6050_ADDR = 0x68;
constexpr uint32_t SERIAL_WAIT_MS = 4000;

// --- Tuning ---
// Knock threshold in g.  Start with 0.15, lower = more sensitive.
constexpr float KNOCK_THRESHOLD_G = 0.15f;
// Minimum ms between two separate knock events (debounce).
constexpr uint32_t KNOCK_COOLDOWN_MS = 1000;
// Polling rate (ms). Faster = better knock detection.
constexpr uint32_t POLL_INTERVAL_MS = 5;
// Periodic status line interval (ms).
constexpr uint32_t STATUS_INTERVAL_MS = 2000;
// Number of samples for baseline calibration at startup.
constexpr uint16_t CALIBRATION_SAMPLES = 200;

// Baseline (gravity-compensated rest values)
static float baseAx = 0, baseAy = 0, baseAz = 0;

static uint32_t knockCount = 0;
static uint32_t lastKnockMs = 0;
static uint32_t lastPollMs = 0;
static uint32_t lastStatusMs = 0;
static float lastDeltaG = 0;
static float peakDeltaG = 0;

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

  // Accel range +-2g (highest sensitivity: 16384 LSB/g)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission(true);

  // DLPF ~44 Hz — filters high-freq noise but keeps knock impulse
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1A);
  Wire.write(0x03);
  Wire.endTransmission(true);

  return true;
}

void calibrateBaseline() {
  Serial.printf("Calibrating baseline (%u samples)... keep still!\n", CALIBRATION_SAMPLES);
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
  Serial.printf("Baseline: ax=%.3f  ay=%.3f  az=%.3f g\n", baseAx, baseAy, baseAz);
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < SERIAL_WAIT_MS) {
    delay(10);
  }
  delay(200);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Serial.println();
  Serial.println("GY-512 (MPU-6050) knock/vibration test");
  Serial.println("Wiring:");
  Serial.printf("  SDA -> GPIO%d\n", I2C_SDA_PIN);
  Serial.printf("  SCL -> GPIO%d\n", I2C_SCL_PIN);
  Serial.println("  VCC -> 3V3");
  Serial.println("  GND -> GND");

  Serial.print("Scanning I2C for 0x68... ");
  Wire.beginTransmission(MPU6050_ADDR);
  if (Wire.endTransmission() == 0) {
    Serial.println("found!");
  } else {
    Serial.println("NOT found! Check wiring.");
  }

  if (initMPU6050()) {
    Serial.println("MPU-6050 initialized (+-2g, DLPF 44Hz)");
  } else {
    Serial.println("MPU-6050 init FAILED");
  }

  delay(100);
  calibrateBaseline();

  Serial.printf("Knock threshold: %.3f g\n", KNOCK_THRESHOLD_G);
  Serial.printf("Cooldown: %lu ms\n", (unsigned long)KNOCK_COOLDOWN_MS);
  Serial.println("Ready. Knock on the plate!");
  Serial.println();
}

void loop() {
  const uint32_t now = millis();
  if (now - lastPollMs < POLL_INTERVAL_MS) return;
  lastPollMs = now;

  int16_t ax, ay, az;
  if (!readAccel(ax, ay, az)) return;

  const float gx = ax / 16384.0f - baseAx;
  const float gy = ay / 16384.0f - baseAy;
  const float gz = az / 16384.0f - baseAz;
  const float delta = sqrtf(gx * gx + gy * gy + gz * gz);
  lastDeltaG = delta;
  if (delta > peakDeltaG) peakDeltaG = delta;

  // Knock detection
  if (delta >= KNOCK_THRESHOLD_G && (now - lastKnockMs) >= KNOCK_COOLDOWN_MS) {
    lastKnockMs = now;
    knockCount++;
    Serial.printf(">>> KNOCK #%lu  delta=%.3fg\n",
                  (unsigned long)knockCount, delta);
  }

  // Periodic status
  if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    Serial.printf("Status: delta=%.4fg  peak=%.3fg  knocks=%lu\n",
                  lastDeltaG, peakDeltaG, (unsigned long)knockCount);
    peakDeltaG = 0;
  }
}
