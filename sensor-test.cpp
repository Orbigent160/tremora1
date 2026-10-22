
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// First I2C bus
#define SDA1 21
#define SCL1 22

// Second I2C bus
#define SDA2 25
#define SCL2 26

#define MPU_ADDR 0x68
#define PCA_ADDR 0x40
#define DRV_ENABLE 27

TwoWire secondBus = TwoWire(1);

struct Vec3 {
  float x, y, z;
};

Vec3 base1 = {0, 0, 0};
Vec3 base2 = {0, 0, 0};

uint8_t adxl1Address = 0;
uint8_t adxl2Address = 0;

bool calibrated1 = false;
bool calibrated2 = false;

bool devicePresent(TwoWire &bus, uint8_t address) {
  bus.beginTransmission(address);
  return bus.endTransmission() == 0;
}

bool writeRegister(TwoWire &bus, uint8_t address,
                   uint8_t reg, uint8_t value) {
  bus.beginTransmission(address);
  bus.write(reg);
  bus.write(value);
  return bus.endTransmission() == 0;
}

bool readRegisters(TwoWire &bus, uint8_t address,
                   uint8_t reg, uint8_t *data, uint8_t len) {
  bus.beginTransmission(address);
  bus.write(reg);

  if (bus.endTransmission(false) != 0)
    return false;

  if (bus.requestFrom(address, len) != len)
    return false;

  for (uint8_t i = 0; i < len; i++)
    data[i] = bus.read();

  return true;
}

// Confirm ADXL345 identity using DEVID register (0xE5)
uint8_t findADXL(TwoWire &bus) {
  uint8_t possibleAddresses[] = {0x53, 0x1D};

  for (uint8_t address : possibleAddresses) {
    uint8_t id = 0;

    if (readRegisters(bus, address, 0x00, &id, 1)
        && id == 0xE5) {
      return address;
    }
  }

  return 0;
}

bool initializeADXL(TwoWire &bus, uint8_t address) {
  return writeRegister(bus, address, 0x2D, 0x00) &&
         writeRegister(bus, address, 0x31, 0x08) &&
         writeRegister(bus, address, 0x2C, 0x0A) &&
         writeRegister(bus, address, 0x2D, 0x08);
}

bool readADXL(TwoWire &bus, uint8_t address, Vec3 &v) {
  uint8_t b[6];

  if (!readRegisters(bus, address, 0x32, b, 6))
    return false;

  int16_t x = (int16_t)((b[1] << 8) | b[0]);
  int16_t y = (int16_t)((b[3] << 8) | b[2]);
  int16_t z = (int16_t)((b[5] << 8) | b[4]);

  v.x = x / 256.0f;
  v.y = y / 256.0f;
  v.z = z / 256.0f;

  return true;
}

bool calibrateADXL(TwoWire &bus, uint8_t address,
                   Vec3 &baseline) {
  Vec3 total = {0, 0, 0};
  const int samples = 100;

  for (int i = 0; i < samples; i++) {
    Vec3 v;

    if (!readADXL(bus, address, v))
      return false;

    total.x += v.x;
    total.y += v.y;
    total.z += v.z;

    delay(10);
  }

  baseline.x = total.x / samples;
  baseline.y = total.y / samples;
  baseline.z = total.z / samples;

  return true;
}

float movement(Vec3 current, Vec3 baseline) {
  float dx = current.x - baseline.x;
  float dy = current.y - baseline.y;
  float dz = current.z - baseline.z;

  return sqrtf(dx*dx + dy*dy + dz*dz);
}

void scanBus(TwoWire &bus, const char *name) {
  Serial.printf("\n%s scan:\n", name);

  int count = 0;

  for (int addr = 0x08; addr <= 0x77; addr++) {
    if (devicePresent(bus, addr)) {
      Serial.printf("  Found 0x%02X\n", addr);
      count++;
    }
  }

  if (count == 0)
    Serial.println("  No devices found");
}

void printADXL(const char *name, TwoWire &bus,
               uint8_t address, Vec3 baseline,
               bool calibrated) {
  if (address == 0 || !devicePresent(bus, address)) {
    Serial.printf("%s: NOT FOUND\n", name);
    return;
  }

  Vec3 v;

  if (!readADXL(bus, address, v)) {
    Serial.printf("%s: READ ERROR\n", name);
    return;
  }

  Serial.printf(
    "%s [0x%02X]: X=%.3f Y=%.3f Z=%.3f g",
    name, address, v.x, v.y, v.z
  );

  if (calibrated) {
    Serial.printf(" | Movement=%.3f g", movement(v, baseline));
  }

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Keep motor drivers disabled during this test
  pinMode(DRV_ENABLE, OUTPUT);
  digitalWrite(DRV_ENABLE, LOW);

  Wire.begin(SDA1, SCL1);
  secondBus.begin(SDA2, SCL2);

  Wire.setClock(100000);
  secondBus.setClock(100000);

  Serial.println("\nDUAL I2C SENSOR TEST");

  scanBus(Wire, "BUS 1 (GPIO21/22)");
  scanBus(secondBus, "BUS 2 (GPIO25/26)");

  adxl1Address = findADXL(Wire);
  adxl2Address = findADXL(secondBus);

  if (adxl1Address != 0) {
    if (!initializeADXL(Wire, adxl1Address))
      adxl1Address = 0;
  }

  if (adxl2Address != 0) {
    if (!initializeADXL(secondBus, adxl2Address))
      adxl2Address = 0;
  }

  // Wake MPU6050
  if (devicePresent(Wire, MPU_ADDR)) {
    writeRegister(Wire, MPU_ADDR, 0x6B, 0x00);
  }

  Serial.println("\nKeep both sensors still...");
  delay(3000);

  if (adxl1Address != 0) {
    calibrated1 = calibrateADXL(
      Wire, adxl1Address, base1
    );
  }

  if (adxl2Address != 0) {
    calibrated2 = calibrateADXL(
      secondBus, adxl2Address, base2
    );
  }

  Serial.printf("ADXL1 calibration: %s\n",
                calibrated1 ? "SUCCESS" : "FAILED");

  Serial.printf("ADXL2 calibration: %s\n",
                calibrated2 ? "SUCCESS" : "FAILED");
}

void loop() {
  Serial.println("\n====== SENSOR STATUS ======");

  Serial.printf("MPU6050: %s\n",
                devicePresent(Wire, MPU_ADDR)
                ? "FOUND" : "NOT FOUND");

  Serial.printf("PCA9685: %s\n",
                devicePresent(Wire, PCA_ADDR)
                ? "FOUND" : "NOT FOUND");

  printADXL("ADXL1", Wire, adxl1Address,
            base1, calibrated1);

  printADXL("ADXL2", secondBus, adxl2Address,
            base2, calibrated2);

  Serial.println("Motors: DISABLED");
  Serial.println("===========================");

  delay(4000);
}
