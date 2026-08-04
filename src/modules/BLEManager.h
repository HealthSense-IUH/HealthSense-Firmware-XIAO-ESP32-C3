#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <stddef.h>
#include <Arduino.h>

void BLEManager_begin();
bool BLEManager_isConnected();
void BLEManager_notify(const char* data, size_t len);
void BLEManager_notifyReport(const char* data, size_t len);
void sendBLECommand(const char* command);
void BLEManager_startAdvertising();
void BLEManager_stopAdvertising();

// Đăng ký hàm xử lý lệnh nhận từ điện thoại qua BLE Write
// Signature: void myHandler(const char* cmd)
void BLEManager_setCommandCallback(void (*callback)(const char* cmd));

// Đọc và cập nhật thời lượng pin (Battery Service 0x180F)
uint8_t BLEManager_readBatteryLevel();
void BLEManager_updateBatteryLevel();

#endif
