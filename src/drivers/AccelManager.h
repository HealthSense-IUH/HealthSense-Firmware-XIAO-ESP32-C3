#ifndef ACCEL_MANAGER_H
#define ACCEL_MANAGER_H

#include <stdint.h>

// Cảm biến gia tốc MPU6050: ngắt vung tay (motion) và đếm bước khi tập
bool AccelManager_begin(uint8_t intPin);
bool AccelManager_isMoving();
// true một lần cho mỗi ngắt chuyển động (đồng thời nhả chân INT đang chốt)
bool AccelManager_popMotionEvent();
// Gọi mỗi vòng loop: đếm bước 25Hz; thử té ngã 100Hz nếu HS_FALL_TEST_ENABLED.
void AccelManager_process();
void AccelManager_printDebug();
void AccelManager_setMotionThreshold(uint8_t threshold);

// Số bước đếm được từ lúc bật máy (peak detection)
uint32_t AccelManager_getStepCount();

// Phát hiện té ngã 3 giai đoạn: Rơi tự do -> Va chạm -> Bất động 1.5s (giống XIAO-nRF52840)
bool AccelManager_popFallEvent();
void AccelManager_startFallDetection();
void AccelManager_stopFallDetection();
bool AccelManager_isFallDetectionEnabled();

#endif
