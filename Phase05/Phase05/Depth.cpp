#include "Depth.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_timer.h>
#include "src/tmf8829/tmf8829.h"
#include "src/tmf8829/tmf8829_fw.h"
#include "BurstConfig.h"
extern "C" void tmf8829TrackClock(tmf8829Driver*, uint32_t, uint32_t);

namespace {
// Camera's bundled SCCB driver owns hardware controller 1 on GPIO8/9.
TwoWire bus(0);
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
namespace {
int depthFailure(const char* reason, const uint8_t* p = nullptr) {
  if (p) Serial.printf("ERROR DEPTH reason=%s fifo=%02x id=%02x layout=%02x length=%u tick=%lu t0=%lu t1=%lu ratio=%u\n",
    reason, p[0], p[5], p[6], tmf8829GetUint16(p+7), (unsigned long)tmf8829GetUint32(p+1),
    (unsigned long)tmf8829GetUint32(p+213), (unsigned long)tmf8829GetUint32(p+217), driver.clkCorrRatioUQ);
  else Serial.printf("ERROR DEPTH reason=%s\n", reason);
  return -1;
}
}
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
  headerOk = (p[5] & 0xf0) == 0x10 && (p[5] & 15) == 0 && p[6] == 1 &&
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
  // Gravity has no EN wire; wake/reset into boot monitor before reading ID.
  // In standby only the always-on register bank is accessible.
  tmf8829Initialise(&driver);
  uint8_t enable = 0x14; // PON, force boot monitor.
  uint8_t reset = 0x40;
  if (txReg(nullptr, addr, 0xf8, 1, &enable)) return false;
  // AN001096 table 4 requires this wake interval before asserting soft reset.
  delay(3);
  if (txReg(nullptr, addr, 0xf7, 1, &reset)) return false;
  delay(10);
  if (!tmf8829IsCpuReady(&driver, 100)) { Serial.println("ERROR depth_boot_cpu"); return false; }
  uint8_t identity[2] = {};
  if (rxReg(nullptr, addr, 0xe3, 2, identity) || identity[0] != 0x9e) {
    Serial.printf("ERROR unexpected_chip id=0x%02x revision=0x%02x\n", identity[0], identity[1]);
    return false;
  }
  Serial.printf("SENSOR address=0x41 sda=%d scl=%d id=%02x revision=%02x\n", sda, scl, identity[0], identity[1]);
  bus.setClock(400000);
  uint8_t app = 0;
  if (rxReg(nullptr, addr, 0, 1, &app) || app != 0x80) {
    Serial.printf("ERROR depth_boot_app app=%02x\n", app); return false;
  }
  Serial.printf("PATCH loading bytes=%u\n", unsigned(sizeof(patchImage)));
  auto checked = [](const char* stage, int result) {
    Serial.printf("DEPTH_INIT stage=%s result=%d io_errors=%lu\n", stage, result, (unsigned long)depthIoErrors);
    return result == 0 && depthIoErrors == 0;
  };
  if (!checked("spi_off", tmf8829BootloaderCmdSpiOff(&driver)) ||
      !checked("patch", tmf8829DownloadFirmware(&driver, TMF8829_IMAGE_START, patchImage, sizeof(patchImage), 0)) ||
      !checked("device_info", tmf8829ReadDeviceInfo(&driver))) return false;
  if (driver.device.appVersion[0] != 1) { Serial.println("ERROR depth_app_version"); return false; }
  if (!checked("preset", tmf8829Command(&driver, 0x41)) ||
      !checked("get_config", tmf8829GetConfiguration(&driver))) return false;
  driver.config[0] = BurstConfig::kDepthPeriodMs & 255;
  driver.config[1] = BurstConfig::kDepthPeriodMs >> 8;
  driver.config[2] = BurstConfig::kDepthIterations & 255;
  driver.config[3] = BurstConfig::kDepthIterations >> 8;
  driver.config[0x2a - 0x22] = 1; // One peak, raw distance and SNR only.
  driver.config[0x2b - 0x22] = 0; // Histograms disabled in this stage.
  driver.config[0x4c - 0x22] = 0; // Dual mode disabled (vendor driver register).
  memcpy(expectedConfig, driver.config, sizeof(expectedConfig));
  if (!checked("set_config", tmf8829SetConfiguration(&driver)) ||
      !checked("readback", tmf8829GetConfiguration(&driver))) return false;
  bool matches = true;
  for (unsigned i = 0; i < sizeof(expectedConfig); ++i) {
    if (expectedConfig[i] == driver.config[i]) continue;
    // DS001140 has no public configuration field at 0x2c. The application
    // normalizes this byte to 1 on write after standby; retain actual readback
    // in metadata, but do not confuse it with a requested setting mismatch.
    if (i + 0x22 == 0x2c) {
      Serial.printf("CONFIG_NORMALIZED reg=2c before=%02x after=%02x\n", expectedConfig[i], driver.config[i]);
      continue;
    }
    matches = false;
    Serial.printf("CONFIG_DIFF reg=%02x expected=%02x actual=%02x\n", i+0x22, expectedConfig[i], driver.config[i]);
  }
  if (!matches || driver.config[4] != 0) {
    Serial.printf("CONFIG_CALIBRATION status=%u\n", driver.config[4]);
    Serial.println("ERROR depth_config_readback"); return false;
  }
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
  uint8_t packet[225] = {};
  const uint64_t begin = esp_timer_get_time();
  if (rxReg(&driver, addr, 0xfa, 21, packet)) return depthFailure("header_io");
  const uint64_t end = esp_timer_get_time();
  if ((packet[5] & 0xf0) != 0x10 || (packet[5] & 15) != 0 || packet[6] != 1 ||
      tmf8829GetUint16(packet + 7) != 216 || (packet[0] & 0x30)) return depthFailure("header", packet);
  tmf8829TrackClock(&driver, uint32_t((begin + end) / 2), tmf8829GetUint32(packet + 1));
  if (rxReg(&driver, addr, 0xff, 204, packet + 21) || tmf8829GetUint16(packet + 223) != 0xe0f7) return depthFailure("body_io_or_eof", packet);
  handleReceivedFrameHeaderData(&driver, packet);
  handleReceivedResultData(&driver, packet + 21, 204);
  handleReceivedResultDataEnd(&driver);
  if (!complete || depthIoErrors != errors) return depthFailure("decode_or_io", packet);
  frame = received;
  frame.hostUs = begin; frame.rxBeginUs = begin; frame.rxEndUs = end;
  memcpy(frame.native, packet, sizeof(packet));
  const uint32_t delta = frame.tick - frame.t0;
  const uint32_t integration = frame.t1 - frame.t0;
  // Reject ambiguous wrap/order or a stale FIFO. At most two configured periods.
  if (delta > 125000 || integration > delta) return depthFailure("tick_order_or_stale", packet);
  const uint64_t agoUs = (uint64_t(delta) * 8 * frame.ratio + 16384) >> 15;
  // Half-percent oscillator/drift margin plus register transaction bracket.
  const uint64_t margin = agoUs / 200 + 1000;
  if (begin < agoUs + margin) return depthFailure("negative_host_time", packet);
  frame.opticalEarliestUs = begin - agoUs - margin;
  frame.opticalLatestUs = end;
  return 1;
}
bool depthStandby() {
  const uint32_t errors = depthIoErrors;
  if (!stopDepth()) return false;
  tmf8829Standby(&driver);
  delay(3);
  uint8_t enable = 0xff;
  if (rxReg(&driver, addr, 0xf8, 1, &enable) || (enable & 0x80)) return false;
  return errors == depthIoErrors;
}
bool validZone(const DepthFrame& f, unsigned i) {
  return (f.status & 1) && !(f.status & 0xc0) && f.raw[i] != 0 && f.snr[i] != 0;
}
void depthMetadata(char* p, size_t n) {
  int used = snprintf(p, n, "{\"sensor\":\"TMF8829\",\"serial\":%lu,\"chip\":[%u,%u],\"app\":[%u,%u,%u,%u],\"sda\":%d,\"scl\":%d,\"i2c_hz\":400000,\"preset\":65,\"period_ms\":%u,\"kilo_iterations\":%u,\"result_format\":1,\"dual_mode\":0,\"histograms\":false,\"config_hex\":\"",
    (unsigned long)driver.device.deviceSerialNumber, driver.device.chipVersion[0], driver.device.chipVersion[1],
    driver.device.appVersion[0], driver.device.appVersion[1], driver.device.appVersion[2], driver.device.appVersion[3],
    sda, scl, unsigned(BurstConfig::kDepthPeriodMs), tmf8829GetUint16(driver.config + 2));
  for (unsigned i = 0; i < sizeof(driver.config) && used > 0 && size_t(used + 4) < n; ++i)
    used += snprintf(p + used, n - used, "%02x", driver.config[i]);
  if (used > 0 && size_t(used + 3) < n) snprintf(p + used, n - used, "\"}");
}
