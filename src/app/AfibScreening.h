#pragma once

// Sàng lọc AFib theo chu kỳ 10 phút (chạy trong MODE_SCREENING của DeviceStateManager).
// Tự bật / tắt MAX30102 theo pha và gửi báo cáo R1 / R2 qua BLE; chi tiết các pha xem AfibScreening.cpp.

// Bắt đầu chu kỳ mới từ pha 1
void AfibScreening_start();
// Dừng (thoát chế độ sàng lọc)
void AfibScreening_stop();
// Gọi mỗi vòng loop khi đang sàng lọc
void AfibScreening_loop();
// Rung tay: chỉ ảnh hưởng pha 1 (ngủ 30s rồi đo lại)
void AfibScreening_onMotion();
