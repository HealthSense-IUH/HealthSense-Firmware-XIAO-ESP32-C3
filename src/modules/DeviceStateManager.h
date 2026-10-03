#pragma once

#include <Arduino.h>
#include "BLEManager.h"
#include "PPGManager.h"
#include "AccelManager.h"
#include "DisplayPower.h"

// Máy trạng thái của thiết bị: chế độ hiện tại, sự kiện từ nút / cảm biến / điện thoại, chu kỳ sàng lọc AFib
enum DeviceMode {
  MODE_IDLE,       // Chờ: cảm biến tắt, dò đeo tay mỗi 3 giây, quảng bá BLE
  MODE_MEASURE,    // Đo chủ động theo lệnh điện thoại: gửi PPG thô
  MODE_WORKOUT,    // Tập luyện: gửi BPM + bước chân mỗi 3 giây, ghi đệm khi mất kết nối
  MODE_SCREENING,  // Sàng lọc AFib theo chu kỳ 10 phút: gửi PPG thô + báo cáo R1/R2
  MODE_SHUTDOWN,   // Tắt máy: deep sleep, giữ nút 3 giây để bật lại
  MODE_TURN_ON     // Vừa bật: nháy LED rồi về IDLE
};

enum DeviceEvent {
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
