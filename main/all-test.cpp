
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ---------- PIN CONFIGURATION ----------
#define SDA1 21
#define SCL1 22

#define SDA2 25
#define SCL2 26

#define DRV_ENABLE 27

// ---------- I2C ADDRESSES ----------
#define MPU_ADDR 0x68
#define PCA_ADDR 0x40

TwoWire secondBus = TwoWire(1);

// ---------- MOVEMENT SETTINGS ----------
const float ACCEL_THRESHOLD = 0.25f;  // g
const float GYRO_THRESHOLD = 15.0f;   // degrees/sec

const unsigned long MOTOR_DURATION_MS = 3000;
const unsigned long QUIET_TIME_MS = 1500;
const unsigned long SAMPLE_MS = 20;

// ---------- SENSOR DATA ----------
struct Vec3 {
  float x, y, z;
};

Vec3 adxl1Base = {0, 0, 0};
Vec3 adxl2Base = {0, 0, 0};
Vec3 gyroBase = {0, 0, 0};

uint8_t adxl1Addr = 0;
uint8_t adxl2Addr = 0;

// ---------- CONTROL STATE ----------
bool motorsActive = false;
bool armed = true;

unsigned long motorStart = 0;
unsigned long quietStart = 0;
unsigned long lastSample = 0;

uint8_t movementCount = 0;

// ---------- I2C HELPERS ----------
bool present(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

bool writeReg(TwoWire &bus, uint8_t addr,
              uint8_t reg, uint8_t value) {
  bus.beginTransmission(addr);
  bus.write(reg);
  bus.write(value);

  return bus.endTransmission() == 0;
}

bool readRegs(TwoWire &bus, uint8_t addr,
              uint8_t reg, uint8_t *data, uint8_t count) {
  bus.beginTransmission(addr);
  bus.write(reg);

  if (bus.endTransmission(false) != 0)
    return false;

  if (bus.requestFrom(addr, count) != count)
    return false;

  for (uint8_t i = 0; i < count; i++) {
    data[i] = bus.read();
  }

  return true;
}

// ---------- PCA9685 MOTOR OUTPUT ----------
bool setPCAOutput(uint8_t output, bool on) {
  uint8_t reg = 0x06 + 4 * output;

  // Full ON / Full OFF using PCA9685 control bits
  if (!writeReg(Wire, PCA_ADDR, reg + 0, 0x00))
    return false;

  if (!writeReg(Wire, PCA_ADDR, reg + 2, 0x00))
    return false;

  if (on) {
    if (!writeReg(Wire, PCA_ADDR, reg + 3, 0x00))
      return false;

    return writeReg(Wire, PCA_ADDR, reg + 1, 0x10);
  } else {
    if (!writeReg(Wire, PCA_ADDR, reg + 3, 0x10))
      return false;

    return writeReg(Wire, PCA_ADDR, reg + 1, 0x00);
  }
}

bool setAllPCA(bool on) {
  for (uint8_t i = 0; i < 6; i++) {
    if (!setPCAOutput(i, on))
      return false;
  }

  return true;
}

bool initializePCA() {
  // Oscillator on, auto-increment enabled
  if (!writeReg(Wire, PCA_ADDR, 0x00, 0x20))
    return false;

  // Totem-pole outputs
  if (!writeReg(Wire, PCA_ADDR, 0x01, 0x04))
    return false;

  return setAllPCA(false);
}

void stopMotors() {
  // Disable drivers first
  digitalWrite(DRV_ENABLE, LOW);
  setAllPCA(false);

  motorsActive = false;
}

void failSafeStop() {
  stopMotors();

  // Stop operation until power is cycled.
  while (true) {
    delay(1000);
  }
}

void startMotors() {
  // Prepare all six outputs before waking drivers
  if (!setAllPCA(true))
    failSafeStop();

  delay(2);

  digitalWrite(DRV_ENABLE, HIGH);

  motorStart = millis();
  motorsActive = true;
  armed = false;
}

// ---------- ADXL345 ----------
uint8_t findADXL(TwoWire &bus) {
  uint8_t addresses[] = {0x53, 0x1D};

  for (uint8_t addr : addresses) {
    uint8_t id = 0;

    if (readRegs(bus, addr, 0x00, &id, 1) &&
        id == 0xE5) {
      return addr;
    }
  }

  return 0;
}

bool initADXL(TwoWire &bus, uint8_t addr) {
  return writeReg(bus, addr, 0x2D, 0x00) &&
         writeReg(bus, addr, 0x31, 0x08) &&
         writeReg(bus, addr, 0x2C, 0x0A) &&
         writeReg(bus, addr, 0x2D, 0x08);
}

bool readADXL(TwoWire &bus, uint8_t addr, Vec3 &v) {
  uint8_t b[6];

  if (!readRegs(bus, addr, 0x32, b, 6))
    return false;

  int16_t x = (int16_t)((b[1] << 8) | b[0]);
  int16_t y = (int16_t)((b[3] << 8) | b[2]);
  int16_t z = (int16_t)((b[5] << 8) | b[4]);

  v.x = x / 256.0f;
  v.y = y / 256.0f;
  v.z = z / 256.0f;

  return true;
}

// ---------- MPU6050 ----------
bool initMPU() {
  return writeReg(Wire, MPU_ADDR, 0x6B, 0x00) &&
         writeReg(Wire, MPU_ADDR, 0x1B, 0x00);
}

bool readMPUGyro(Vec3 &v) {
  uint8_t b[6];

  if (!readRegs(Wire, MPU_ADDR, 0x43, b, 6))
    return false;

  v.x = (int16_t)((b[0] << 8) | b[1]) / 131.0f;
  v.y = (int16_t)((b[2] << 8) | b[3]) / 131.0f;
  v.z = (int16_t)((b[4] << 8) | b[5]) / 131.0f;

  return true;
}

// ---------- MOVEMENT CALCULATION ----------
float difference(Vec3 a, Vec3 b) {
  float dx = a.x - b.x;
  float dy = a.y - b.y;
  float dz = a.z - b.z;

  return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool readAll(Vec3 &a1, Vec3 &a2, Vec3 &gyro) {
  return readADXL(Wire, adxl1Addr, a1) &&
         readADXL(secondBus, adxl2Addr, a2) &&
         readMPUGyro(gyro);
}

// ---------- CALIBRATION ----------
bool calibrate() {
  Vec3 sum1 = {0, 0, 0};
  Vec3 sum2 = {0, 0, 0};
  Vec3 sumG = {0, 0, 0};

  const int samples = 150;

  for (int i = 0; i < samples; i++) {
    Vec3 a, b, g;

    if (!readAll(a, b, g))
      return false;

    sum1.x += a.x;
    sum1.y += a.y;
    sum1.z += a.z;

    sum2.x += b.x;
    sum2.y += b.y;
    sum2.z += b.z;

    sumG.x += g.x;
    sumG.y += g.y;
    sumG.z += g.z;

    delay(10);
  }

  adxl1Base = {
    sum1.x / samples,
    sum1.y / samples,
    sum1.z / samples
  };

  adxl2Base = {
    sum2.x / samples,
    sum2.y / samples,
    sum2.z / samples
  };

  gyroBase = {
    sumG.x / samples,
    sumG.y / samples,
    sumG.z / samples
  };

  return true;
}

// ---------- SETUP ----------
void setup() {
  // Keep DRV8833 disabled immediately
  pinMode(DRV_ENABLE, OUTPUT);
  digitalWrite(DRV_ENABLE, LOW);

  Wire.begin(SDA1, SCL1);
  secondBus.begin(SDA2, SCL2);

  Wire.setClock(100000);
  secondBus.setClock(100000);

  Wire.setTimeOut(50);
  secondBus.setTimeOut(50);

  // Require all expected devices
  if (!present(Wire, MPU_ADDR) ||
      !present(Wire, PCA_ADDR)) {
    failSafeStop();
  }

  adxl1Addr = findADXL(Wire);
  adxl2Addr = findADXL(secondBus);

  if (adxl1Addr == 0 || adxl2Addr == 0)
    failSafeStop();

  if (!initializePCA())
    failSafeStop();

  if (!initMPU() ||
      !initADXL(Wire, adxl1Addr) ||
      !initADXL(secondBus, adxl2Addr)) {
    failSafeStop();
  }

  // Motors remain OFF during calibration
  delay(3000);

  if (!calibrate())
    failSafeStop();

  lastSample = millis();
}

// ---------- MAIN LOOP ----------
void loop() {
  // Stop exactly after the intended pulse interval
  if (motorsActive &&
      millis() - motorStart >= MOTOR_DURATION_MS) {
    stopMotors();
    quietStart = 0;
  }

  if (millis() - lastSample < SAMPLE_MS)
    return;

  lastSample = millis();

  Vec3 a1, a2, gyro;

  if (!readAll(a1, a2, gyro)) {
    failSafeStop();
  }

  // Ignore motion-triggering while motors are running
  if (motorsActive)
    return;

  float movement1 = difference(a1, adxl1Base);
  float movement2 = difference(a2, adxl2Base);
  float rotation = difference(gyro, gyroBase);

  bool movementDetected =
      movement1 > ACCEL_THRESHOLD ||
      movement2 > ACCEL_THRESHOLD ||
      rotation > GYRO_THRESHOLD;

  // Wait for sensors to settle after a motor pulse
  if (!armed) {
    if (movementDetected) {
      quietStart = 0;
    } else {
      if (quietStart == 0)
        quietStart = millis();

      if (millis() - quietStart >= QUIET_TIME_MS) {
        armed = true;
        movementCount = 0;
      }
    }

    return;
  }

  // Require 3 consecutive readings to avoid noise spikes
  if (movementDetected) {
    movementCount++;

    if (movementCount >= 3) {
      movementCount = 0;
      startMotors();
    }
  } else {
    movementCount = 0;
  }
}
