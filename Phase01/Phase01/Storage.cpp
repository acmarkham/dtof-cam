#include "Storage.h"
#include "BoardConfig.h"
#include <SPI.h>
#include <SdFat.h>
#include <cstring>

namespace {
SPIClass sdSpi(FSPI);
SdFs sd;
bool mounted = false;
uint64_t volumeBytes = 0;
uint64_t freeBytesAtBoot = 0;

void imagePath(char* path, size_t size, uint32_t sequence, const char* suffix) {
  snprintf(path, size, "/%06lu.%s", static_cast<unsigned long>(sequence), suffix);
}

bool sequenceFromName(const char* name, uint32_t& sequence) {
  if (strlen(name) != 10 || name[6] != '.') return false;
  if (strcasecmp(name + 7, "jpg") != 0 && strcasecmp(name + 7, "tmp") != 0) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < 6; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    value = value * 10 + (name[i] - '0');
  }
  sequence = value;
  return true;
}

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

bool initStorage(uint32_t& nextSequence) {
  sdSpi.begin(Board::kSdSck, Board::kSdMiso, Board::kSdMosi, Board::kSdCs);
  // The SD card is the only SPI device on these board pins.
  if (!sd.begin(SdSpiConfig(Board::kSdCs, DEDICATED_SPI, SD_SCK_MHZ(10), &sdSpi))) {
    sd.initErrorPrint(&Serial);
    Serial.println("ERROR SD mount failed; card has NOT been formatted");
    return false;
  }
  const uint8_t type = sd.fatType();
  if (type != 64 && !(Board::kAllowFat32 && type == 32)) {
    Serial.printf("ERROR unsupported filesystem fatType=%u; exFAT required\n", type);
    return false;
  }
  const uint32_t freeClusters = sd.freeClusterCount();
  if (freeClusters == UINT32_MAX) {
    Serial.println("ERROR cannot read SD free space");
    return false;
  }
  const uint64_t clusterBytes = uint64_t(sd.sectorsPerCluster()) * 512ULL;
  volumeBytes = uint64_t(sd.clusterCount()) * clusterBytes;
  freeBytesAtBoot = uint64_t(freeClusters) * clusterBytes;
  cid_t cid = {};
  if (!sd.card()->readCID(&cid)) {
    Serial.println("ERROR cannot read SD identity");
    return false;
  }
  Serial.printf("SD filesystem=%s capacity_bytes=%llu free_bytes=%llu spi_mhz=10 cluster_bytes=%llu cid=",
                type == 64 ? "exFAT" : "FAT32", volumeBytes, freeBytesAtBoot, clusterBytes);
  const uint8_t* rawCid = reinterpret_cast<const uint8_t*>(&cid);
  for (size_t i = 0; i < sizeof(cid); ++i) Serial.printf("%02x", rawCid[i]);
  Serial.println();
  FsFile root;
  if (!root.open("/", O_RDONLY)) return false;
  FsFile entry;
  uint32_t highest = 0;
  while (entry.openNext(&root, O_RDONLY)) {
    char name[32] = {};
    uint32_t sequence = 0;
    if (!entry.isDir() && entry.getName(name, sizeof(name)) && sequenceFromName(name, sequence)) {
      if (sequence > highest) highest = sequence;
    }
    if (!entry.close()) return false;
    delay(0);
  }
  const bool scanOk = !root.getError();
  const bool closeOk = root.close();
  if (!scanOk || !closeOk) return false;
  nextSequence = highest + 1;
  mounted = true;
  Serial.printf("SEQUENCE next=%06lu\n", static_cast<unsigned long>(nextSequence));
  return true;
}

SaveResult saveImage(uint32_t sequence, const uint8_t* data, size_t length) {
  if (!mounted) return {false, "not_mounted"};
  if (sequence == 0 || sequence > Board::kLastSequence) return {false, "sequence_exhausted"};
  char temporary[16], finalPath[16];
  imagePath(temporary, sizeof(temporary), sequence, "tmp");
  imagePath(finalPath, sizeof(finalPath), sequence, "jpg");
  // Exclusive creation and SdFat's exclusive destination rename protect both
  // names. Avoid redundant linear directory scans as the image count grows.
  FsFile file;
  if (!file.open(temporary, O_WRONLY | O_CREAT | O_EXCL)) return {false, "create_failed"};
  const size_t written = file.write(data, length);
  const bool syncOk = file.sync();
  const bool closeOk = file.close();
  if (written != length) return {false, "short_write"};
  if (!syncOk || !closeOk) return {false, "sync_close_failed"};
  if (!sd.rename(temporary, finalPath)) return {false, "rename_failed"};
  return {true, "none"};
}

void printStorageStatus() {
  Serial.printf("STORAGE mounted=%u filesystem=%s capacity_bytes=%llu free_bytes_at_boot=%llu\n",
                mounted, mounted ? (sd.fatType() == 64 ? "exFAT" : "FAT32") : "none",
                volumeBytes, freeBytesAtBoot);
}

void downloadImage(uint32_t sequence) {
  char path[16];
  imagePath(path, sizeof(path), sequence, "jpg");
  FsFile file;
  if (!mounted || !file.open(path, O_RDONLY)) {
    Serial.println("ERROR GET open_failed");
    return;
  }
  const uint64_t length = file.fileSize();
  // Restrict diagnostics to bounded JPEG transfers, never arbitrary SD files.
  if (length > 4ULL * 1024 * 1024) {
    file.close();
    Serial.println("ERROR GET image_too_large");
    return;
  }
  char header[96];
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
