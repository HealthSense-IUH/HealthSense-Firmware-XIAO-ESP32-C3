#ifndef PPG_MANAGER_H
#define PPG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

bool PPGManager_begin(uint8_t interruptPin);
void PPGManager_process();
// Lấy gói PPG đã gom đủ 10 mẫu (false nếu chưa đủ)
bool PPGManager_popPacket(char* outBuf, size_t bufSize, size_t* outLen);
// Bỏ gói đang gom (chế độ không gửi PPG thô, tránh đầy bộ đệm)
void PPGManager_discardPacket();
uint8_t PPGManager_getBPM();
uint8_t PPGManager_getSpO2();
void PPGManager_handleInterrupt();
void PPGManager_wakeUp();
void PPGManager_shutDown();
bool PPGManager_popNoFingerEvent();
void PPGManager_setupPhase1();
void PPGManager_setupPhase2(bool lowPower);
long PPGManager_readIR();

#endif
