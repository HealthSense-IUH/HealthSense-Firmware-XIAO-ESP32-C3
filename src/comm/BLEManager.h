#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <stddef.h>
#include <Arduino.h>

// Kết nối BLE với điện thoại; giao thức (UUID, lệnh, định dạng gói) xem BleProtocol.h
void BLEManager_begin();
// Gọi mỗi vòng loop: gửi bù dữ liệu offline sau khi kết nối lại
void BLEManager_loop();
bool BLEManager_isConnected();
// Gói PPG thô (characteristic DATA)
void BLEManager_notify(const char* data, size_t len);
// Báo cáo R1/R2/W (characteristic REPORT)
void BLEManager_notifyReport(const char* data, size_t len);
// Sự kiện CMD:/ACK:/ERR: lên điện thoại (characteristic DATA), bỏ qua khi chưa kết nối
void BLEManager_sendEvent(const char* message);
void BLEManager_startAdvertising();

// Đăng ký hàm xử lý lệnh nhận từ điện thoại qua BLE Write. Signature: void handler(const char* cmd)
void BLEManager_setCommandCallback(void (*callback)(const char* cmd));

// Đọc mức pin và cập nhật Battery Service (0x180F)
void BLEManager_updateBatteryLevel();

// Ghi đệm mẫu tập luyện khi mất kết nối BLE
void BLEManager_pushOfflineSample(uint8_t bpm, uint32_t steps);

#endif
