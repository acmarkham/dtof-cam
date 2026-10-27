#include "BurstStorage.h"
#include "BurstConfig.h"
#include "RgbStream.h"
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
uint32_t number = 0, indexCrc = 0;
uint64_t indexBytes = 0;
uint8_t uuidBytes[16];
char path[40], uuid[37], textBuffer[8192];
uint32_t crc(const uint8_t* p, size_t n, uint32_t previous = 0) { return esp_rom_crc32_le(previous, p, n); }
bool append(size_t& used, const char* format, ...) {
  va_list args; va_start(args, format);
  const int n = vsnprintf(textBuffer + used, sizeof(textBuffer) - used, format, args);
  va_end(args);
  if (n < 0 || size_t(n) >= sizeof(textBuffer) - used) return false;
  used += n; return true;
}
class StreamFile {
  FsFile file;
public:
  uint32_t checksum = 0;
  uint64_t bytes = 0;
  bool begin(const char* name) {
    char temporary[64]; snprintf(temporary, sizeof(temporary), "%s.tmp", name);
    return file.open(&directory, temporary, O_CREAT | O_EXCL | O_WRONLY);
  }
  bool write(const void* data, size_t n) {
    if (file.write(data, n) != n) return false;
    checksum = crc(static_cast<const uint8_t*>(data), n, checksum); bytes += n;
    return true;
  }
  bool finish(const char* name) {
    const bool synced = file.sync();
    const bool renamed = synced && file.rename(&directory, name);
    const bool closed = file.close();
    return synced && renamed && closed;
  }
};
bool writeCommitted(const char* name, const uint8_t* bytes, size_t n) {
  StreamFile file; return file.begin(name) && file.write(bytes, n) && file.finish(name);
}
bool writeText(const char* name) { return writeCommitted(name, reinterpret_cast<uint8_t*>(textBuffer), strlen(textBuffer)); }
bool appendIndex(const char* data, size_t n) {
  if (indexFile.write(data, n) != n || !indexFile.sync()) return false;
  indexCrc = crc(reinterpret_cast<const uint8_t*>(data), n, indexCrc); indexBytes += n; return true;
}
void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (unsigned i=0; i<4; ++i) p[i] = v >> (8*i); }
void put64(uint8_t* p, uint64_t v) { for (unsigned i=0; i<8; ++i) p[i] = v >> (8*i); }
bool writeDepth(const BurstStats& stats, const DepthFrame* frames, uint32_t& csvCrc,
                uint64_t& csvBytes, uint32_t& binaryCrc, uint64_t& binaryBytes) {
  StreamFile csv, binary;
  if (!csv.begin("depth.csv") || !binary.begin("depth.bin")) return false;
  size_t used = 0;
  if (!append(used, "# phase05_depth_csv=1\n# burst_id=%s\n# clock=esp_timer_us_since_boot\nframe_seq,sensor_frame_no,host_rx_us,rx_end_us,optical_earliest_us,optical_latest_us,sensor_tick_raw,t0_integration_raw,t1_integration_raw,clock_ratio_uq15,frame_status_raw", uuid)) return false;
  for (unsigned z=0; z<64; ++z) if (!append(used, ",raw_qmm_%02u,snr_%02u,distance_qmm_%02u,valid_%02u", z,z,z,z)) return false;
  if (!append(used, "\n") || !csv.write(textBuffer, used)) return false;
  uint8_t header[64] = {};
  memcpy(header, "D5TOF1\0\0", 8); put16(header+8, 1); put16(header+10, 64);
  put32(header+12, 288); put32(header+16, stats.depthCount); put32(header+20, BurstConfig::kDepthIterations);
  put64(header+24, stats.startUs); put64(header+32, stats.startUs+BurstConfig::kBurstUs);
  memcpy(header+40, uuidBytes, 16); put32(header+60, crc(header, 60));
  if (!binary.write(header, sizeof(header))) return false;
  for (uint32_t i=0; i<stats.depthCount; ++i) {
    const auto& d = frames[i]; used = 0;
    if (!append(used, "%lu,%lu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%u,%u", (unsigned long)(i+1),
        (unsigned long)d.number, d.hostUs, d.rxEndUs, d.opticalEarliestUs, d.opticalLatestUs,
        (unsigned long)d.tick, (unsigned long)d.t0, (unsigned long)d.t1, d.ratio, d.status)) return false;
    for (unsigned z=0; z<64; ++z) {
      if (!append(used, ",%u,%u,", d.raw[z], d.snr[z])) return false;
      if (validZone(d,z) && !append(used, "%u", d.corrected[z])) return false;
      if (!append(used, ",%u", validZone(d,z))) return false;
    }
    if (!append(used, "\n") || !csv.write(textBuffer, used)) return false;
    uint8_t record[288] = {};
    put32(record, i+1); put32(record+4, d.number); put64(record+8, d.hostUs); put64(record+16, d.rxEndUs);
    put64(record+24, d.opticalEarliestUs); put64(record+32, d.opticalLatestUs);
    put32(record+40, d.tick); put32(record+44, d.t0); put32(record+48, d.t1);
    put16(record+52, d.ratio); record[54] = d.status; memcpy(record+56, d.native, 225);
    put32(record+284, crc(record, 284));
    if (!binary.write(record, sizeof(record))) return false;
  }
  csvCrc = csv.checksum; csvBytes = csv.bytes; binaryCrc = binary.checksum; binaryBytes = binary.bytes;
  return csv.finish("depth.csv") && binary.finish("depth.bin");
}
}

uint32_t burstNumber() { return number; }
bool beginBurstStorage(uint32_t wakeReason, uint32_t bootCount) {
  if (crc(reinterpret_cast<const uint8_t*>("123456789"), 9) != 0xcbf43926) return false;
  spi.begin(12, 13, 11, 10);
  if (!sd.begin(SdSpiConfig(10, DEDICATED_SPI, SD_SCK_MHZ(BurstConfig::kSdClockMHz), &spi))) { sd.initErrorPrint(&Serial); return false; }
  if (sd.fatType() != 64 && sd.fatType() != 32) return false;
  if (!sd.exists("/phase05") && !sd.mkdir("/phase05")) return false;
  for (number=1; number<=999999; ++number) {
    snprintf(path, sizeof(path), "/phase05/b%06lu", (unsigned long)number);
    if (!sd.exists(path)) break;
  }
  if (number>999999 || !sd.mkdir(path) || !directory.open(path, O_RDONLY)) return false;
  esp_fill_random(uuidBytes, sizeof(uuidBytes)); uuidBytes[6] = (uuidBytes[6]&15)|64; uuidBytes[8] = (uuidBytes[8]&63)|128;
  const auto& b = uuidBytes;
  snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
  char camera[768], depth[2048]; rgbMetadata(camera, sizeof(camera)); depthMetadata(depth, sizeof(depth));
  snprintf(textBuffer, sizeof(textBuffer),
    "{\"schema\":1,\"firmware\":\"Phase05\",\"burst_id\":\"%s\",\"burst_number\":%lu,\"duration_us\":%lu,\"depth_fps\":%u,\"rgb_fps\":%u,\"sleep_seconds\":%lu,\"wake_reason\":%lu,\"boot_count\":%lu,\"filesystem\":\"%s\",\"clock\":\"esp_timer_us_since_boot\",\"rgb\":%s,\"depth\":%s,\"depth_durability\":\"RAM during burst; checked SD commit after capture\",\"depth_binary\":\"D5TOF1/v1; header64 record288 little-endian\",\"vendor_commit\":\"%s\",\"patch_sha256\":\"%s\"}\n",
    uuid, (unsigned long)number, (unsigned long)BurstConfig::kBurstUs, BurstConfig::kDepthFps, BurstConfig::kRgbFps,
    (unsigned long)BurstConfig::kSleepSeconds, (unsigned long)wakeReason, (unsigned long)bootCount,
    sd.fatType()==64 ? "exFAT" : "FAT32", camera, depth, TMF_VENDOR_COMMIT, TMF_PATCH_SHA256);
  if (!writeText("burst.json") || !indexFile.open(&directory, "rgb.part", O_CREAT | O_EXCL | O_WRONLY)) return false;
  const char* header = "frame_seq,camera_dma_us,camera_done_us,camera_period_bound_us,optical_earliest_us,optical_latest_us,scheduled_us,jpeg_bytes,jpeg_crc32\n";
  if (!appendIndex(header, strlen(header))) return false;
  mounted = true; storageStatus(); return true;
}
bool saveRgb(uint32_t sequence, const uint8_t* jpeg, const RgbFrame& rgb, uint64_t dueUs, uint64_t& writeUs) {
  const uint64_t start = esp_timer_get_time();
  if (!mounted || !sequence) return false;
  char name[32]; snprintf(name, sizeof(name), "%06lu.jpg", (unsigned long)sequence);
  if (!writeCommitted(name, jpeg, rgb.length)) return false;
  const int n = snprintf(textBuffer, sizeof(textBuffer), "%lu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%08lx\n",
    (unsigned long)sequence, rgb.dmaUs, rgb.doneUs, rgb.periodBoundUs, rgb.earliestUs, rgb.latestUs, dueUs,
    unsigned(rgb.length), (unsigned long)crc(jpeg, rgb.length));
  if (n<0 || size_t(n)>=sizeof(textBuffer) || !appendIndex(textBuffer,n)) return false;
  writeUs = esp_timer_get_time()-start; return true;
}
bool finishBurstStorage(const BurstStats& s, const DepthFrame* frames) {
  if (!mounted || !indexFile.sync() || !indexFile.rename(&directory,"rgb.csv") || !indexFile.close()) return false;
  uint32_t depthCsvCrc=0, depthBinaryCrc=0; uint64_t depthCsvBytes=0, depthBinaryBytes=0;
  const uint64_t flushStart = esp_timer_get_time();
  if (!writeDepth(s,frames,depthCsvCrc,depthCsvBytes,depthBinaryCrc,depthBinaryBytes)) return false;
  snprintf(textBuffer,sizeof(textBuffer),
    "{\"complete\":true,\"success\":%s,\"rgb_frames\":%lu,\"depth_frames\":%lu,\"depth_gaps\":%lu,\"depth_overflows\":%lu,\"rgb_missed\":%lu,\"rgb_errors\":%lu,\"depth_errors\":%lu,\"capture_start_us\":%llu,\"capture_window_end_us\":%llu,\"capture_stop_us\":%llu,\"max_rgb_write_us\":%llu,\"depth_flush_us\":%llu,\"rgb_index_bytes\":%llu,\"rgb_index_crc32\":\"%08lx\",\"depth_csv_bytes\":%llu,\"depth_csv_crc32\":\"%08lx\",\"depth_binary_bytes\":%llu,\"depth_binary_crc32\":\"%08lx\",\"sensor_standby\":%s,\"camera_off\":%s,\"led\":0}\n",
    s.success ? "true" : "false", (unsigned long)s.rgbCount, (unsigned long)s.depthCount, (unsigned long)s.depthGaps,
    (unsigned long)s.depthOverflows, (unsigned long)s.rgbMissed, (unsigned long)s.rgbErrors, (unsigned long)s.depthErrors,
    s.startUs,s.startUs+BurstConfig::kBurstUs,s.stopUs,s.maxWriteUs,uint64_t(esp_timer_get_time())-flushStart,
    indexBytes,(unsigned long)indexCrc,depthCsvBytes,(unsigned long)depthCsvCrc,depthBinaryBytes,(unsigned long)depthBinaryCrc,
    s.sensorStandby ? "true" : "false",s.cameraOff ? "true" : "false");
  return writeText("summary.json");
}
bool closeBurstStorage() {
  if (!mounted) return false;
  if (indexFile.isOpen() && !indexFile.close()) return false;
  if (!directory.close() || !sd.card()->syncDevice()) return false;
  sd.end(); spi.end(); mounted = false; return true;
}
void storageStatus() { Serial.printf("STORAGE mounted=%u burst=%lu path=%s uuid=%s\n", mounted,(unsigned long)number,path,uuid); }

namespace {
bool awaitAck(const char* expected) {
  char response[48] = {}; size_t used=0; const uint32_t start=millis();
  while (millis()-start<1000) {
    while (Serial.available()) {
      const char c=Serial.read(); if(c=='\r') continue;
      if(c=='\n') { response[used]=0; if(!strcmp(response,expected)) return true; used=0; }
      else if(used<sizeof(response)-1) response[used++]=c;
    }
    delay(1);
  }
  return false;
}
bool sendPacket(const char* p,size_t n,const char* ack) {
  for(unsigned retry=0;retry<8;++retry) {
    size_t sent=0; const uint32_t start=millis();
    while(sent<n && millis()-start<3000) { sent+=Serial.write(reinterpret_cast<const uint8_t*>(p)+sent,n-sent); delay(1); }
    Serial.flush(); if(sent==n && awaitAck(ack)) return true;
  }
  return false;
}
}
void downloadBurstFile(uint32_t burst,const char* kind,uint32_t sequence) {
  if(!mounted || !burst || burst>999999) { Serial.println("ERROR GET not_mounted_or_bad_burst"); return; }
  char name[40],full[96];
  if(!strcmp(kind,"META")) strcpy(name,"burst.json");
  else if(!strcmp(kind,"INDEX")) strcpy(name,"rgb.csv");
  else if(!strcmp(kind,"SUMMARY")) strcpy(name,"summary.json");
  else if(!strcmp(kind,"DEPTH")) strcpy(name,"depth.csv");
  else if(!strcmp(kind,"RAW")) strcpy(name,"depth.bin");
  else if(!strcmp(kind,"JPG") && sequence>=1 && sequence<=30) snprintf(name,sizeof(name),"%06lu.jpg",(unsigned long)sequence);
  else { Serial.println("ERROR GET bad_file"); return; }
  snprintf(full,sizeof(full),"/phase05/b%06lu/%s",(unsigned long)burst,name);
  FsFile f; if(!f.open(full,O_RDONLY)) { Serial.println("ERROR GET open_failed"); return; }
  const uint64_t length=f.fileSize();
  if(length>2*1024*1024) { f.close(); Serial.println("ERROR GET too_large"); return; }
  char packet[1152],ack[48];
  const int header=snprintf(packet,sizeof(packet),"\nFILE path=%s bytes=%llu\n",full,length);
  if(!sendPacket(packet,header,"ACK FILE")) { f.close(); Serial.println("END_FILE ok=0"); return; }
  uint8_t data[512]; uint64_t offset=0; int n=0;
  constexpr char hex[]="0123456789abcdef";
  while((n=f.read(data,sizeof(data)))>0) {
    const int prefix=snprintf(packet,sizeof(packet),"\nDATA offset=%llu bytes=%u crc32=%08lx hex=",offset,n,(unsigned long)crc(data,n));
    for(int i=0;i<n;++i) { packet[prefix+2*i]=hex[data[i]>>4]; packet[prefix+2*i+1]=hex[data[i]&15]; }
    packet[prefix+2*n]='\n'; snprintf(ack,sizeof(ack),"ACK %llu",offset);
    if(!sendPacket(packet,prefix+2*n+1,ack)) { n=-1; break; }
    offset+=n;
  }
  const bool ok=n==0 && offset==length && !f.getError(); const bool closed=f.close();
  Serial.printf("END_FILE ok=%u bytes=%llu\n",ok && closed,offset); Serial.flush();
}
