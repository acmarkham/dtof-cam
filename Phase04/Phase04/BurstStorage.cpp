#include "BurstStorage.h"
#include "BurstConfig.h"
#include "RgbStream.h"
#include "BoardConfig.h"
#include "src/tmf8829/Provenance.h"
#include <SPI.h>
#include <SdFat.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_rom_crc.h>
#include <stdarg.h>

namespace {
SPIClass spi(FSPI);
SdFs sd;
FsFile directory, indexFile;
bool mounted = false;
uint32_t number = 0;
char path[40], uuid[37], textBuffer[8192];
uint32_t crc(const uint8_t* p, size_t n) { return esp_rom_crc32_le(0, p, n); }
bool append(size_t& used, const char* format, ...) {
  va_list args; va_start(args, format);
  const int n = vsnprintf(textBuffer + used, sizeof(textBuffer) - used, format, args);
  va_end(args);
  if (n < 0 || size_t(n) >= sizeof(textBuffer) - used) return false;
  used += n; return true;
}
bool writeCommitted(const char* name, const uint8_t* data, size_t length) {
  char tmp[64]; snprintf(tmp, sizeof(tmp), "%s.tmp", name);
  FsFile f;
  if (!f.open(&directory, tmp, O_CREAT | O_EXCL | O_WRONLY)) return false;
  const bool written = f.write(data, length) == length;
  const bool synced = f.sync(), closed = f.close();
  if (!written || !synced || !closed || !f.open(&directory, tmp, O_WRONLY)) return false;
  const bool renamed = f.rename(&directory, name);
  const bool closedAgain = f.close();
  return renamed && closedAgain;
}
bool writeText(const char* name) { return writeCommitted(name, reinterpret_cast<uint8_t*>(textBuffer), strlen(textBuffer)); }
}

uint32_t burstNumber() { return number; }
bool beginBurstStorage(uint64_t start, uint32_t wakeReason, uint32_t bootCount) {
  if (crc(reinterpret_cast<const uint8_t*>("123456789"), 9) != 0xcbf43926) return false;
  spi.begin(12, 13, 11, 10);
  if (!sd.begin(SdSpiConfig(10, DEDICATED_SPI, SD_SCK_MHZ(BurstConfig::kSdClockMHz), &spi))) { sd.initErrorPrint(&Serial); return false; }
  if (sd.fatType() != 64 && sd.fatType() != 32) return false;
  if (!sd.exists("/bursts") && !sd.mkdir("/bursts")) return false;
  for (number = 1; number <= 999999; ++number) {
    snprintf(path, sizeof(path), "/bursts/b%06lu", (unsigned long)number);
    if (!sd.exists(path)) break;
  }
  if (number > 999999 || !sd.mkdir(path) || !directory.open(path, O_RDONLY)) return false;
  uint8_t b[16]; esp_fill_random(b, sizeof(b)); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
  snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
  char camera[768], depth[2048]; rgbMetadata(camera, sizeof(camera)); depthMetadata(depth, sizeof(depth));
  snprintf(textBuffer, sizeof(textBuffer),
    "{\"schema\":1,\"firmware\":\"Phase04\",\"burst_id\":\"%s\",\"burst_number\":%lu,\"planned_start_us\":%llu,\"duration_us\":%lu,\"target_fps\":%u,\"max_fps\":10,\"period_us\":%lu,\"sync_tolerance_us\":%lu,\"sleep_seconds\":%lu,\"wake_reason\":%lu,\"boot_count\":%lu,\"filesystem\":\"%s\",\"rgb\":%s,\"depth\":%s,\"vendor_commit\":\"%s\",\"patch_sha256\":\"%s\",\"sync_contract\":\"worst separation of conservative optical-time envelopes <= half configured pair period\",\"depth_mapping_margin\":\"0.5 percent of tick age plus 1000us, with I2C read bracket\",\"rgb_exposure_bound\":\"manual exposure <= VTS; one observed sensor period before first DMA through buffer return\"}\n",
    uuid, (unsigned long)number, start, (unsigned long)BurstConfig::kBurstUs, BurstConfig::kFps,
    (unsigned long)BurstConfig::kPeriodUs, (unsigned long)BurstConfig::kToleranceUs,
    (unsigned long)BurstConfig::kSleepSeconds, (unsigned long)wakeReason, (unsigned long)bootCount,
    sd.fatType() == 64 ? "exFAT" : "FAT32", camera, depth, TMF_VENDOR_COMMIT, TMF_PATCH_SHA256);
  if (!writeText("burst.json") || !indexFile.open(&directory, "pairs.part", O_CREAT | O_EXCL | O_WRONLY)) return false;
  const char* header = "pair_seq,depth_frame_no,camera_dma_us,camera_done_us,camera_period_bound_us,rgb_earliest_us,rgb_latest_us,depth_t0_tick,depth_t1_tick,depth_systick,depth_rx_begin_us,depth_rx_end_us,clock_ratio_uq15,depth_earliest_us,depth_latest_us,midpoint_delta_us,sync_bound_us,sync_tolerance_us,jpeg_bytes,jpeg_crc32,depth_csv_bytes,depth_csv_crc32,native_crc32\n";
  const size_t n = strlen(header);
  if (indexFile.write(header, n) != n || !indexFile.sync()) return false;
  mounted = true; storageStatus(); return true;
}
bool savePair(uint32_t sequence, const uint8_t* jpeg, const RgbFrame& rgb, const DepthFrame& d,
              const PairTiming& timing, uint64_t& writeUs) {
  const uint64_t started = esp_timer_get_time();
  if (!mounted || !sequence || timing.worstSeparationUs > BurstConfig::kToleranceUs) return false;
  const uint32_t jpegCrc = crc(jpeg, rgb.length), nativeCrc = crc(d.native, sizeof(d.native));
  char name[32]; snprintf(name, sizeof(name), "%06lu.jpg", (unsigned long)sequence);
  if (!writeCommitted(name, jpeg, rgb.length)) return false;
  size_t used = 0;
  if (!append(used, "# dtof_csv_version=1\n# session_id=%s\n# config_id=1\n# frame_seq=%lu\n# sensor_frame_no=%lu\n# host_rx_us=%llu\n# sensor_tick_raw=%lu\n# clock_ratio_uq15=%u\n# frame_status_raw=%u\n# t0_integration_raw=%lu\n# t1_integration_raw=%lu\n# histogram_expected=0\n# histogram_received=0\n# histogram_complete=0\n# width=8,height=8,peaks=1\nx,y,distance_mm,distance_raw_qmm,snr_raw,valid\n",
      uuid, (unsigned long)sequence, (unsigned long)d.number, d.hostUs, (unsigned long)d.tick, d.ratio,
      d.status, (unsigned long)d.t0, (unsigned long)d.t1)) return false;
  for (unsigned i = 0; i < 64; ++i) {
    if (validZone(d, i)) {
      if (!append(used, "%u,%u,%u.%03u,%u,%u,1\n", i%8, i/8, d.corrected[i]/4,
                  (d.corrected[i]%4)*250, d.raw[i], d.snr[i])) return false;
    } else if (!append(used, "%u,%u,,%u,%u,0\n", i%8, i/8, d.raw[i], d.snr[i])) return false;
  }
  const uint32_t depthCrc = crc(reinterpret_cast<uint8_t*>(textBuffer), used);
  snprintf(name, sizeof(name), "%06lu.csv", (unsigned long)sequence);
  if (!writeCommitted(name, reinterpret_cast<uint8_t*>(textBuffer), used)) return false;
  snprintf(name, sizeof(name), "%06lu.tof", (unsigned long)sequence);
  if (!writeCommitted(name, d.native, sizeof(d.native))) return false;
  const int n = snprintf(textBuffer, sizeof(textBuffer),
    "%lu,%lu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%llu,%llu,%u,%llu,%llu,%lld,%llu,%lu,%u,%08lx,%u,%08lx,%08lx\n",
    (unsigned long)sequence, (unsigned long)d.number, rgb.dmaUs, rgb.doneUs, rgb.periodBoundUs,
    rgb.earliestUs, rgb.latestUs, (unsigned long)d.t0, (unsigned long)d.t1, (unsigned long)d.tick,
    d.rxBeginUs, d.rxEndUs, d.ratio, timing.depthEarliestUs, timing.depthLatestUs, timing.midpointDeltaUs,
    timing.worstSeparationUs, (unsigned long)BurstConfig::kToleranceUs, unsigned(rgb.length),
    (unsigned long)jpegCrc, unsigned(used), (unsigned long)depthCrc, (unsigned long)nativeCrc);
  if (n < 0 || size_t(n) >= sizeof(textBuffer) || indexFile.write(textBuffer, n) != size_t(n) || !indexFile.sync()) return false;
  writeUs = esp_timer_get_time() - started;
  return true;
}
bool finishBurstStorage(const BurstStats& s) {
  if (!mounted || !indexFile.sync() || !indexFile.rename(&directory, "pairs.csv") || !indexFile.close()) return false;
  snprintf(textBuffer, sizeof(textBuffer),
    "{\"complete\":true,\"success\":%s,\"pairs\":%lu,\"sync_rejected\":%lu,\"depth_gaps\":%lu,\"queue_drops\":%lu,\"rgb_errors\":%lu,\"depth_errors\":%lu,\"capture_start_us\":%llu,\"capture_stop_us\":%llu,\"capture_duration_us\":%llu,\"max_sync_bound_us\":%llu,\"sync_tolerance_us\":%lu,\"max_write_us\":%llu,\"sensor_standby\":%s,\"camera_off\":%s,\"led\":0}\n",
    s.success ? "true" : "false", (unsigned long)s.pairs, (unsigned long)s.syncRejected,
    (unsigned long)s.depthGaps, (unsigned long)s.queueDrops, (unsigned long)s.rgbErrors,
    (unsigned long)s.depthErrors, s.startUs, s.stopUs, s.stopUs - s.startUs, s.maxBoundUs,
    (unsigned long)BurstConfig::kToleranceUs, s.maxWriteUs, s.sensorStandby ? "true" : "false", s.cameraOff ? "true" : "false");
  return writeText("summary.json");
}
bool closeBurstStorage() {
  if (!mounted) return false;
  if (indexFile.isOpen() && !indexFile.close()) return false;
  if (!directory.close() || !sd.card()->syncDevice()) return false;
  sd.end(); spi.end(); mounted = false; return true;
}
void storageStatus() { Serial.printf("STORAGE mounted=%u burst=%lu path=%s uuid=%s\n", mounted, (unsigned long)number, path, uuid); }

namespace {
bool awaitAck(const char* expected) {
  char response[48] = {}; size_t used = 0; const uint32_t start = millis();
  while (millis() - start < 1000) {
    while (Serial.available()) {
      const char c = Serial.read(); if (c == '\r') continue;
      if (c == '\n') { response[used] = 0; if (!strcmp(response, expected)) return true; used = 0; }
      else if (used < sizeof(response)-1) response[used++] = c;
    }
    delay(1);
  }
  return false;
}
bool sendPacket(const char* p, size_t n, const char* ack) {
  for (unsigned retry = 0; retry < 8; ++retry) {
    size_t sent = 0; const uint32_t start = millis();
    while (sent < n && millis()-start < 3000) { sent += Serial.write(reinterpret_cast<const uint8_t*>(p)+sent, n-sent); delay(1); }
    Serial.flush(); if (sent == n && awaitAck(ack)) return true;
  }
  return false;
}
}
void downloadBurstFile(uint32_t burst, const char* kind, uint32_t sequence) {
  if (!mounted || !burst || burst > 999999) { Serial.println("ERROR GET not_mounted_or_bad_burst"); return; }
  char name[40], full[96];
  if (!strcmp(kind, "META")) strcpy(name, "burst.json");
  else if (!strcmp(kind, "INDEX")) strcpy(name, "pairs.csv");
  else if (!strcmp(kind, "SUMMARY")) strcpy(name, "summary.json");
  else {
    const char* ext = !strcmp(kind,"JPG") ? "jpg" : !strcmp(kind,"DEPTH") ? "csv" : !strcmp(kind,"RAW") ? "tof" : nullptr;
    if (!ext || !sequence || sequence > 300) { Serial.println("ERROR GET bad_file"); return; }
    snprintf(name, sizeof(name), "%06lu.%s", (unsigned long)sequence, ext);
  }
  snprintf(full, sizeof(full), "/bursts/b%06lu/%s", (unsigned long)burst, name);
  FsFile f; if (!f.open(full, O_RDONLY)) { Serial.println("ERROR GET open_failed"); return; }
  const uint64_t length = f.fileSize();
  if (length > 2*1024*1024) { f.close(); Serial.println("ERROR GET too_large"); return; }
  char packet[1152], ack[48];
  const int header = snprintf(packet, sizeof(packet), "\nFILE path=%s bytes=%llu\n", full, length);
  if (!sendPacket(packet, header, "ACK FILE")) { f.close(); Serial.println("END_FILE ok=0"); return; }
  uint8_t data[512]; uint64_t offset = 0; int n = 0;
  constexpr char hex[] = "0123456789abcdef";
  while ((n = f.read(data, sizeof(data))) > 0) {
    const int prefix = snprintf(packet, sizeof(packet), "\nDATA offset=%llu bytes=%u crc32=%08lx hex=", offset, n, (unsigned long)crc(data,n));
    for (int i = 0; i < n; ++i) { packet[prefix+2*i] = hex[data[i]>>4]; packet[prefix+2*i+1] = hex[data[i]&15]; }
    packet[prefix+2*n] = '\n'; snprintf(ack, sizeof(ack), "ACK %llu", offset);
    if (!sendPacket(packet, prefix+2*n+1, ack)) { n = -1; break; }
    offset += n;
  }
  const bool ok = n == 0 && offset == length && !f.getError(); const bool closed = f.close();
  Serial.printf("END_FILE ok=%u bytes=%llu\n", ok && closed, offset); Serial.flush();
}
