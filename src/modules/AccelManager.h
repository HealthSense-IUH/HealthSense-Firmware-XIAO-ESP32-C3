#ifndef ACCEL_MANAGER_H
#define ACCEL_MANAGER_H

#include <Adafruit_Sensor.h>

bool AccelManager_begin(uint8_t intPin);
bool AccelManager_isReady();
bool AccelManager_isMoving();
bool AccelManager_popMotionEvent();
void AccelManager_process();
void AccelManager_printDebug();
void AccelManager_setMotionThreshold(uint8_t threshold);

#endif
