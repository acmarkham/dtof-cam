#include "Depth.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_timer.h>
#include "src/tmf8829/tmf8829.h"
#include "src/tmf8829/tmf8829_fw.h"

namespace {
TwoWire bus(1);
tmf8829Driver driver;
int sda = 43, scl = 44;
constexpr uint8_t addr = 0x41;
constexpr unsigned chunk = 128;
DepthFrame received;
bool complete = false;
uint8_t header[21];
uint8_t body[204]; // 64 x (quarter-mm u16 + SNR u8), footer.
uint16_t bodyUsed = 0;
bool headerOk = false;
uint8_t expectedConfig[TMF8829_CFG_PAGE_SIZE];
bool probe(int data, int clock) {
  bus.end();
  sda = data; scl = clock;
  if (!bus.begin(sda, scl, 100000)) return false;
  bus.setTimeOut(50);
  bus.setBufferSize(256);
  bool found = false;
  Serial.printf("SCAN sda=%d scl=%d hz=100000\n", sda, scl);
  for (uint8_t a = 8; a < 120; ++a) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) {
      Serial.printf("I2C address=0x%02x\n", a);
      if (a == addr) found = true;
    }
    delay(1);
  }
  return found;
}
}
uint32_t depthIoErrors = 0;
extern "C" {
void delayInMicroseconds(uint32_t wait) { delay(wait / 1000); delayMicroseconds(wait % 1000); }
uint32_t getSysTick() { return uint32_t(esp_timer_get_time()); }
uint8_t readProgramMemoryByte(uintptr_t address) { return *reinterpret_cast<const uint8_t*>(address); }
int enablePinHigh(void*) { return 0; } // EN is physically held high; no Gravity EN wire.
int enablePinLow(void*) { return -1; }
int8_t txReg(void*, uint8_t a, uint8_t reg, uint16_t n, uint8_t* p) {
  if (n > 255) { ++depthIoErrors; return -1; }
  bus.beginTransmission(a);
  bus.write(reg);
  const size_t written = bus.write(p, n);
  const uint8_t error = bus.endTransmission();
  if (written != n || error) { ++depthIoErrors; return -1; }
  return 0;
}
int8_t rxReg(void*, uint8_t a, uint8_t reg, uint16_t n, uint8_t* p) {
  // Register windows increment normally; FIFO bytes always come from 0xFF.
  // A read spanning 0xFA..0xFE continues at the FIFO, which never increments.
  uint16_t offset = 0;
  while (offset < n) {
    const uint8_t r = reg == 0xff ? 0xff : uint8_t(reg + offset);
    const uint16_t count = min(unsigned(n - offset), chunk);
    bus.beginTransmission(a); bus.write(r);
    if (bus.endTransmission(false) || bus.requestFrom(a, size_t(count), true) != count) {
      memset(p + offset, 0, n - offset);
      ++depthIoErrors; return -1;
    }
    for (unsigned i = 0; i < count; ++i) p[offset + i] = bus.read();
    offset += count;
  }
  return 0;
}
void handleReceivedFrameHeaderData(void*, uint8_t* p) {
  memcpy(header, p, sizeof(header));
  bodyUsed = 0; complete = false;
  headerOk = (p[5] & 0xf0) == 0x10 && (p[5] & 15) <= 1 && p[6] == 1 &&
             tmf8829GetUint16(p + 7) == 216 && !(p[0] & 0x30);
  received.hostUs = esp_timer_get_time();
}
void handleReceivedResultData(void*, uint8_t* p, uint16_t n) {
  if (!headerOk || bodyUsed + n > sizeof(body)) { headerOk = false; return; }
  memcpy(body + bodyUsed, p, n); bodyUsed += n;
}
void handleReceivedResultDataEnd(void*) {
  if (!headerOk || bodyUsed != sizeof(body) || tmf8829GetUint16(body + 202) != 0xe0f7) return;
  received.number = tmf8829GetUint32(header + 9);
  received.tick = tmf8829GetUint32(header + 1);
  received.t0 = tmf8829GetUint32(body + 192);
  received.t1 = tmf8829GetUint32(body + 196);
  received.status = body[200];
  received.ratio = driver.clkCorrRatioUQ;
  for (unsigned i = 0; i < 64; ++i) {
    received.raw[i] = tmf8829GetUint16(body + i * 3);
    received.snr[i] = body[i * 3 + 2];
    received.corrected[i] = tmf8829CorrectDistance(&driver, received.raw[i]);
  }
  complete = true;
}
void handleReceivedHistogramData(void*, uint8_t*, uint16_t) {}
void handleReceivedHistogramDataEnd(void*) {}
}
bool beginDepth() {
  if (!probe(43, 44) && !probe(44, 43)) {
    Serial.println("ERROR sensor_not_found address=0x41 check_3V3_GND_SDA_SCL_EN_I2C_jumpers");
    return false;
  }
  uint8_t identity[2] = {};
  if (rxReg(nullptr, addr, 0xe3, 2, identity) || identity[0] != 0x9e) {
    Serial.printf("ERROR unexpected_chip id=0x%02x revision=0x%02x\n", identity[0], identity[1]);
    return false;
  }
  Serial.printf("SENSOR address=0x41 sda=%d scl=%d id=%02x revision=%02x\n", sda, scl, identity[0], identity[1]);
  bus.setClock(400000);
  tmf8829Initialise(&driver);
  uint8_t app = 0;
  if (rxReg(nullptr, addr, 0, 1, &app)) return false;
  if (app == 1) { // Soft reset makes warm ESP restarts deterministic without an EN wire.
    tmf8829StopMeasurement(&driver);
    tmf8829Reset(&driver); delay(10);
  }
  tmf8829PowerUp(&driver);
  delay(5);
  if (!tmf8829IsCpuReady(&driver, 20)) return false;
  if (rxReg(nullptr, addr, 0, 1, &app) || app != 0x80) return false;
  Serial.printf("PATCH loading bytes=%u\n", unsigned(sizeof(patchImage)));
  if (tmf8829BootloaderCmdSpiOff(&driver) ||
      tmf8829DownloadFirmware(&driver, TMF8829_IMAGE_START, patchImage, sizeof(patchImage), 0) ||
      tmf8829ReadDeviceInfo(&driver) || driver.device.appVersion[0] != 1) return false;
  if (tmf8829Command(&driver, 0x41) || tmf8829GetConfiguration(&driver)) return false;
  driver.config[0] = 0xe8; driver.config[1] = 3; // Sensor period: 1000 ms.
  driver.config[0x2a - 0x22] = 1; // One peak, raw distance and SNR only.
  driver.config[0x2b - 0x22] = 0; // Histograms disabled in this stage.
  driver.config[0x4c - 0x22] = 0; // Dual mode disabled (vendor driver register).
  memcpy(expectedConfig, driver.config, sizeof(expectedConfig));
  if (tmf8829SetConfiguration(&driver) || tmf8829GetConfiguration(&driver) ||
      memcmp(expectedConfig, driver.config, sizeof(expectedConfig)) ||
      driver.config[4] > 1 || depthIoErrors) return false;
  tmf8829ClkCorrection(&driver, 0); // Retain raw data; update ratio, correct a separate copy.
  tmf8829ClrAndEnableInterrupts(&driver, TMF8829_APP_INT_RESULTS);
  char metadata[2048]; depthMetadata(metadata, sizeof(metadata)); Serial.println(metadata);
  return depthIoErrors == 0;
}
bool startDepth() {
  const uint32_t errors = depthIoErrors;
  return tmf8829StartMeasurement(&driver) == 0 && depthIoErrors == errors;
}
bool stopDepth() {
  const uint32_t errors = depthIoErrors;
  return tmf8829StopMeasurement(&driver) == 0 && depthIoErrors == errors;
}
int pollDepth(DepthFrame& frame) {
  const uint32_t errors = depthIoErrors;
  if (!(tmf8829GetAndClrInterrupts(&driver, 1) & 1)) return depthIoErrors == errors ? 0 : -1;
  complete = false;
  const int result = tmf8829ReadResults(&driver);
  if (result || !complete || errors != depthIoErrors) return -1;
  frame = received; return 1;
}
bool validZone(const DepthFrame& f, unsigned i) {
  return (f.status & 1) && !(f.status & 0xc0) && f.raw[i] != 0 && f.snr[i] != 0;
}
void depthMetadata(char* p, size_t n) {
  int used = snprintf(p, n, "{\"sensor\":\"TMF8829\",\"serial\":%lu,\"chip\":[%u,%u],\"app\":[%u,%u,%u,%u],\"sda\":%d,\"scl\":%d,\"i2c_hz\":400000,\"preset\":65,\"period_ms\":1000,\"kilo_iterations\":%u,\"result_format\":1,\"dual_mode\":0,\"histograms\":false,\"config_hex\":\"",
    (unsigned long)driver.device.deviceSerialNumber, driver.device.chipVersion[0], driver.device.chipVersion[1],
    driver.device.appVersion[0], driver.device.appVersion[1], driver.device.appVersion[2], driver.device.appVersion[3],
    sda, scl, tmf8829GetUint16(driver.config + 2));
  for (unsigned i = 0; i < sizeof(driver.config) && used > 0 && size_t(used + 4) < n; ++i)
    used += snprintf(p + used, n - used, "%02x", driver.config[i]);
  if (used > 0 && size_t(used + 3) < n) snprintf(p + used, n - used, "\"}");
}
