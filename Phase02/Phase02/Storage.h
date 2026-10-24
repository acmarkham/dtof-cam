#pragma once
#include "Depth.h"
bool beginStorage();
bool saveDepth(uint32_t sequence, const DepthFrame& frame, size_t& bytes);
void downloadDepth(uint32_t sequence);
void printStorageStatus();
