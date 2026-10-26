#include "Storage.h"
#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <esp_random.h>
#include <stdarg.h>
#include "src/tmf8829/Provenance.h"

namespace {
SPIClass sdSpi(FSPI);
SdFs sd;
FsFile sessionDir, csvDir, bucketDir;
bool mounted = false;
uint32_t bucket = UINT32_MAX;
char sessionPath[40], sessionId[37];
char content[8192];
void imagePath(char* path, size_t size, uint32_t seq, const char* suffix) {
  snprintf(path, size, "%s/csv/b%04lu/%09lu.%s", sessionPath,
           (unsigned long)((seq - 1) / 1000), (unsigned long)seq, suffix);
}
bool writeExclusive(FsFile& dir, const char* name, const char* data) {
  FsFile file;
  if (!file.open(&dir, name, O_WRONLY | O_CREAT | O_EXCL)) return false;
  const size_t length = strlen(data);
  const bool written = file.write(data, length) == length;
  const bool synced = file.sync();
  const bool closed = file.close();
  return written && synced && closed;
}
bool append(size_t& used, const char* format, ...) {
  va_list args; va_start(args, format);
  const int n = vsnprintf(content + used, sizeof(content) - used, format, args);
  va_end(args);
  if (n < 0 || size_t(n) >= sizeof(content) - used) return false;
  used += n; return true;
}
}

bool beginStorage() {
  sdSpi.begin(12, 13, 11, 10);
  if (!sd.begin(SdSpiConfig(10, DEDICATED_SPI, SD_SCK_MHZ(10), &sdSpi))) {
    sd.initErrorPrint(&Serial); return false;
  }
  if (sd.fatType() != 64 && sd.fatType() != 32) return false;
  Serial.printf("SD filesystem=%s capacity_bytes=%llu\n", sd.fatType() == 64 ? "exFAT" : "FAT32",
                uint64_t(sd.clusterCount()) * sd.sectorsPerCluster() * 512ULL);
  if (!sd.exists("/dtof") && !sd.mkdir("/dtof")) return false;
  // Only session names are examined, never the camera's growing root JPEG list.
  uint32_t number = 1;
  for (; number <= 999999; ++number) {
    snprintf(sessionPath, sizeof(sessionPath), "/dtof/s%06lu", (unsigned long)number);
    if (!sd.exists(sessionPath)) break;
  }
  if (number > 999999 || !sd.mkdir(sessionPath) || !sessionDir.open(sessionPath, O_RDONLY)) return false;
  uint8_t uuid[16]; esp_fill_random(uuid, sizeof(uuid));
  uuid[6] = (uuid[6] & 15) | 0x40; uuid[8] = (uuid[8] & 63) | 0x80;
  snprintf(sessionId, sizeof(sessionId), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    uuid[0],uuid[1],uuid[2],uuid[3],uuid[4],uuid[5],uuid[6],uuid[7],uuid[8],uuid[9],uuid[10],uuid[11],uuid[12],uuid[13],uuid[14],uuid[15]);
  char path[80];
  snprintf(path, sizeof(path), "%s/config", sessionPath);
  FsFile configDir;
  if (!sd.mkdir(path) || !configDir.open(path, O_RDONLY)) return false;
  char metadata[2048]; depthMetadata(metadata, sizeof(metadata));
  snprintf(content, sizeof(content), "{\"config_id\":1,\"effective_sensor\":%s,\"preset_name\":\"8X8_LONG_RANGE\",\"requested_fps\":0.2,\"clock_correction\":\"vendor UQ1.15 rounded to quarter-mm\",\"host_tick_hz\":1000000,\"sensor_tick_hz\":125000,\"validity\":\"frame bit0 set, bits6/7 clear, raw distance and SNR nonzero\",\"orientation\":\"native vendor row-major index=y*8+x, no transform\",\"calibration\":\"sensor factory calibration; no host calibration loaded\",\"calibration_hash\":null}\n", metadata);
  if (!writeExclusive(configDir, "000001.json", content) || !configDir.close()) return false;
  const uint64_t mac = ESP.getEfuseMac();
  char macText[18];
  snprintf(macText, sizeof(macText), "%02x:%02x:%02x:%02x:%02x:%02x",
    unsigned(mac & 255), unsigned((mac >> 8) & 255), unsigned((mac >> 16) & 255),
    unsigned((mac >> 24) & 255), unsigned((mac >> 32) & 255), unsigned((mac >> 40) & 255));
  snprintf(content, sizeof(content), "{\"session_id\":\"%s\",\"schema\":1,\"firmware\":\"Phase03\",\"board\":\"DFRobot ESP32-S3 AI Camera v1.1\",\"board_mac\":\"%s\",\"sensor\":\"TMF8829\",\"driver_commit\":\"%s\",\"patch_sha256\":\"%s\",\"driver_version\":\"1.4\",\"host_protocol\":\"AN001096 v3-00\",\"arduino_esp32\":\"3.3.2\",\"sdfat\":\"2.3.1\",\"config_id\":1,\"timestamp_origin\":\"boot monotonic microseconds; no UTC clock\",\"storage\":\"%s\",\"histograms\":true}\n",
    sessionId, macText, TMF_VENDOR_COMMIT, TMF_PATCH_SHA256, sd.fatType() == 64 ? "exFAT" : "FAT32");
  if (!writeExclusive(sessionDir, "session.json", content)) return false;
  snprintf(path, sizeof(path), "%s/csv", sessionPath);
  if (!sd.mkdir(path) || !csvDir.open(path, O_RDONLY)) return false;
  mounted = true;
  printStorageStatus();
  return true;
}

bool saveDepth(uint32_t seq, const DepthFrame& f, size_t& bytes) {
  if (!mounted || !seq || seq > 999999999) return false;
  const uint32_t wantedBucket = (seq - 1) / 1000;
  if (bucket != wantedBucket) {
    if (bucketDir.isOpen() && !bucketDir.close()) return false;
    char name[16]; snprintf(name, sizeof(name), "b%04lu", (unsigned long)wantedBucket);
    if (csvDir.exists(name) || !bucketDir.mkdir(&csvDir, name, false)) return false;
    bucket = wantedBucket;
  }
  size_t used = 0;
  if (!append(used, "# dtof_csv_version=1\n# session_id=%s\n# config_id=1\n# frame_seq=%lu\n# sensor_frame_no=%lu\n# host_rx_us=%llu\n# sensor_tick_raw=%lu\n# clock_ratio_uq15=%u\n# frame_status_raw=%u\n# t0_integration_raw=%lu\n# t1_integration_raw=%lu\n# histogram_expected=2\n# histogram_received=2\n# histogram_complete=1\n# width=8,height=8,peaks=1\n",
      sessionId, (unsigned long)seq, (unsigned long)f.number, f.hostUs, (unsigned long)f.tick,
      f.ratio, f.status, (unsigned long)f.t0, (unsigned long)f.t1)) return false;
  if (!append(used, "# group_start_us=%llu\n# histogram_frame_0=%lu\n# histogram_frame_1=%lu\n# histogram_crc32_0=%08lx\n# histogram_crc32_1=%08lx\n# native_depth_crc32=%08lx\n",
      f.groupStartUs, (unsigned long)f.histogramNumbers[0], (unsigned long)f.histogramNumbers[1],
      (unsigned long)f.histogramCrc[0], (unsigned long)f.histogramCrc[1], (unsigned long)f.depthCrc)) return false;
  if (!append(used, "x,y,distance_mm,distance_raw_qmm,snr_raw,valid\n")) return false;
  for (unsigned i = 0; i < 64; ++i) {
    if (validZone(f, i)) {
      if (!append(used, "%u,%u,%u.%03u,%u,%u,1\n", i % 8, i / 8,
                  f.corrected[i] / 4, (f.corrected[i] % 4) * 250, f.raw[i], f.snr[i])) return false;
    } else if (!append(used, "%u,%u,,%u,%u,0\n", i % 8, i / 8, f.raw[i], f.snr[i])) return false;
  }
  char temporary[20], finalName[20];
  snprintf(temporary, sizeof(temporary), "%09lu.tmp", (unsigned long)seq);
  snprintf(finalName, sizeof(finalName), "%09lu.csv", (unsigned long)seq);
  if (!writeExclusive(bucketDir, temporary, content)) return false;
  // FsFile::rename checks destination exclusivity; same cached directory.
  FsFile file;
  if (!file.open(&bucketDir, temporary, O_WRONLY)) return false;
  const bool renamed = file.rename(&bucketDir, finalName);
  const bool closed = file.close();
  bytes = used;
  return renamed && closed;
}

void printStorageStatus() {
  Serial.printf("STORAGE mounted=%u session=%s session_id=%s\n", mounted, sessionPath, sessionId);
}

namespace {
bool sendText(const char* data, size_t length) {
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < length && millis() - started < 3000) {
    sent += Serial.write(reinterpret_cast<const uint8_t*>(data) + sent, length - sent);
    delay(1);
  }
  Serial.flush();
  return sent == length;
}

bool awaitAck(const char* expected) {
  char response[40] = {};
  size_t used = 0;
  const uint32_t started = millis();
  while (millis() - started < 1000) {
    while (Serial.available()) {
      const char value = static_cast<char>(Serial.read());
      if (value == '\r') continue;
      if (value == '\n') {
        response[used] = 0;
        if (!strcmp(response, expected)) return true;
        used = 0;
      } else if (used < sizeof(response) - 1) response[used++] = value;
    }
    delay(1);
  }
  return false;
}

bool sendAcknowledged(const char* text, size_t length, const char* ack) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (sendText(text, length) && awaitAck(ack)) return true;
  }
  return false;
}
}


uint32_t imageCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}


void downloadDepth(uint32_t sequence) {
  char path[96];
  if (sequence == 0) snprintf(path, sizeof(path), "%s/session.json", sessionPath);
  else if (sequence == UINT32_MAX) snprintf(path, sizeof(path), "%s/config/000001.json", sessionPath);
  else imagePath(path, sizeof(path), sequence, "csv");
  FsFile file;
  if (!mounted || !file.open(path, O_RDONLY)) {
    Serial.println("ERROR GET open_failed");
    return;
  }
  const uint64_t length = file.fileSize();
  // Bound current-session CSV transfers.
  if (length > 8192) {
    file.close();
    Serial.println("ERROR GET csv_too_large");
    return;
  }
  char header[144];
  snprintf(header, sizeof(header), "\nFILE path=%s bytes=%llu\n", path, length);
  if (!sendAcknowledged(header, strlen(header), "ACK FILE")) {
    file.close();
    Serial.println("END_FILE ok=0 error=header_ack_timeout");
    return;
  }
  uint8_t buffer[512];
  char packet[1152];
  constexpr char digits[] = "0123456789abcdef";
  uint64_t total = 0;
  int count = 0;
  while ((count = file.read(buffer, sizeof(buffer))) > 0) {
    const int prefix = snprintf(packet, sizeof(packet), "\nDATA offset=%llu bytes=%u crc32=%08lx hex=",
                                total, unsigned(count), static_cast<unsigned long>(imageCrc32(buffer, count)));
    for (int i = 0; i < count; ++i) {
      packet[prefix + 2 * i] = digits[buffer[i] >> 4];
      packet[prefix + 2 * i + 1] = digits[buffer[i] & 15];
    }
    packet[prefix + 2 * count] = '\n';
    char ack[40];
    snprintf(ack, sizeof(ack), "ACK %llu", total);
    if (!sendAcknowledged(packet, prefix + 2 * count + 1, ack)) { count = -1; break; }
    total += count;
  }
  const bool readOk = count == 0 && !file.getError() && total == length;
  const bool closeOk = file.close();
  Serial.printf("END_FILE ok=%u bytes=%llu\n", readOk && closeOk, total);
  Serial.flush();
}

void downloadNative(uint32_t sequence, unsigned part) {
  size_t length = 0;
  const uint8_t* data = findDepthSet(sequence, part, length);
  if (!data) { Serial.println("ERROR GET RAW not_in_cache"); return; }
  char header[120];
  snprintf(header, sizeof(header), "\nFILE path=ram/%09lu/%u.native bytes=%u\n", (unsigned long)sequence, part, unsigned(length));
  if (!sendAcknowledged(header, strlen(header), "ACK FILE")) {
    Serial.println("END_FILE ok=0 error=header_ack_timeout"); return;
  }
  constexpr char digits[] = "0123456789abcdef";
  char packet[1152], ack[40];
  for (size_t offset = 0; offset < length; offset += 512) {
    const size_t count = min(size_t(512), length - offset);
    const int prefix = snprintf(packet, sizeof(packet), "\nDATA offset=%u bytes=%u crc32=%08lx hex=",
      unsigned(offset), unsigned(count), (unsigned long)histogramCrc32(data + offset, count));
    for (size_t i = 0; i < count; ++i) {
      packet[prefix + 2 * i] = digits[data[offset + i] >> 4];
      packet[prefix + 2 * i + 1] = digits[data[offset + i] & 15];
    }
    packet[prefix + 2 * count] = '\n';
    snprintf(ack, sizeof(ack), "ACK %u", unsigned(offset));
    if (!sendAcknowledged(packet, prefix + 2 * count + 1, ack)) {
      Serial.printf("END_FILE ok=0 bytes=%u\n", unsigned(offset)); return;
    }
  }
  Serial.printf("END_FILE ok=1 bytes=%u\n", unsigned(length)); Serial.flush();
}
