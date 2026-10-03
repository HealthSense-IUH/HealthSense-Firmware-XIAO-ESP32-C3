#ifndef ACCEL_MANAGER_H
#define ACCEL_MANAGER_H

#include <stdint.h>

// Cảm biến gia tốc MPU6050: ngắt vung tay (motion) và đếm bước khi tập
bool AccelManager_begin(uint8_t intPin);
bool AccelManager_isMoving();
// true một lần cho mỗi ngắt chuyển động (đồng thời nhả chân INT đang chốt)
bool AccelManager_popMotionEvent();
// Gọi mỗi vòng loop: lấy mẫu đếm bước 25Hz
void AccelManager_process();
void AccelManager_printDebug();
void AccelManager_setMotionThreshold(uint8_t threshold);

// Số bước đếm được từ lúc bật máy (peak detection)
uint32_t AccelManager_getStepCount();

#endif
