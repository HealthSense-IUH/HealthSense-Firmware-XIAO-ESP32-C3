#pragma once

#include <Arduino.h>
#include "BLEManager.h"
#include "PPGManager.h"
#include "AccelManager.h"
#include "DisplayPower.h"

enum DeviceMode{
    MODE_IDLE,
    MODE_MEASURE,
    MODE_WORKOUT,
    MODE_SCREENING,
    MODE_SHUTDOWN,
    MODE_TURN_ON
};

enum DeviceEvent{
    EVT_BUTTON_SHORT,
    EVT_BUTTON_LONG,
    EVT_MOTION,
    EVT_NOT_WEARING,
    EVT_BLE_START_MEASURE,
    EVT_BLE_START_WORKOUT,
    EVT_BLE_START_SCREENING
};

void DeviceStateManager_begin(uint8_t buttonPin, uint8_t accelVddPin);
void DeviceStateManager_handleButton();
void DeviceStateManager_onEvent(DeviceEvent event);
void DeviceStateManager_loop();
DeviceMode DeviceStateManager_getMode();
