#pragma once
#include <Arduino.h>

struct SaveResult {
  bool ok;
  const char* error;
};
bool initStorage(uint32_t& nextSequence);
SaveResult saveImage(uint32_t sequence, const uint8_t* data, size_t length);
void printStorageStatus();
void downloadImage(uint32_t sequence);
uint32_t imageCrc32(const uint8_t* data, size_t length);
