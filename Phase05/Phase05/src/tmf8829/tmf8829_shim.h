// Arduino adapter for the ams-OSRAM MIT-licensed TMF8829 core.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DATA_BUFFER_SIZE 1024
#define PTR_TO_UINT(ptr) ((uintptr_t)(ptr))
#define PRINT_CHAR(c) printf("%c", c)
#define PRINT_INT(i) printf("%ld", (long)(i))
#define PRINT_UINT(i) printf("%lu", (unsigned long)(i))
#define PRINT_UINT_HEX(i) printf("%lX", (unsigned long)(i))
#define PRINT_STR(s) printf("%s", s)
#define PRINT_LN() printf("\n")
#define SEPARATOR ','
#define HOST_TICKS_PER_1000_US 1000
#define TMF8829_TICKS_PER_1000_US 125
#define I2C_SUCCESS 0
#define I2C_ERROR -1
#ifdef __cplusplus
extern "C" {
#endif
void delayInMicroseconds(uint32_t wait);
uint32_t getSysTick(void);
uint8_t readProgramMemoryByte(uintptr_t address);
int8_t txReg(void*, uint8_t, uint8_t, uint16_t, uint8_t*);
int8_t rxReg(void*, uint8_t, uint8_t, uint16_t, uint8_t*);
int enablePinHigh(void*);
int enablePinLow(void*);
void handleReceivedFrameHeaderData(void*, uint8_t*);
void handleReceivedResultData(void*, uint8_t*, uint16_t);
void handleReceivedResultDataEnd(void*);
void handleReceivedHistogramData(void*, uint8_t*, uint16_t);
void handleReceivedHistogramDataEnd(void*);
#ifdef __cplusplus
}
#endif
