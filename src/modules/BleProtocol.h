// Giao thức BLE giữa thiết bị và ứng dụng điện thoại. App phải dùng đúng các chuỗi / UUID này.
#pragma once

// ---- Service / characteristic ----
#define BLE_DEVICE_NAME_PREFIX   "HuyWatch_"  // App quét theo tiền tố này; tên đầy đủ thêm 2 byte cuối MAC
#define BLE_SERVICE_UUID         "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_DATA_CHAR_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // NOTIFY: PPG thô + sự kiện (CMD:/ACK:/ERR:)
#define BLE_COMMAND_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a9"  // WRITE: lệnh từ điện thoại
#define BLE_REPORT_CHAR_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26aa"  // NOTIFY: báo cáo R1/R2/W, dữ liệu offline
#define BLE_BATTERY_SERVICE_UUID 0x180F  // Battery Service chuẩn Bluetooth SIG
#define BLE_BATTERY_LEVEL_UUID   0x2A19

// ---- Lệnh điện thoại -> thiết bị (characteristic WRITE, bỏ \r\n cuối) ----
#define CMD_START_MEASURE    "CMD:START_MEASURE"
#define CMD_START_WORKOUT    "CMD:START_WORKOUT"
#define CMD_WORKOUT_START    "CMD:WORKOUT_START"   // Tên cũ của START_WORKOUT, vẫn nhận
#define CMD_WORKOUT_PAUSE    "CMD:WORKOUT_PAUSE"
#define CMD_WORKOUT_RESUME   "CMD:WORKOUT_RESUME"
#define CMD_WORKOUT_STOP     "CMD:WORKOUT_STOP"
#define CMD_START_SCREENING  "CMD:START_SCREENING"
#define CMD_IDLE             "CMD:IDLE"

// ---- Sự kiện thiết bị -> điện thoại (characteristic DATA) ----
#define EVT_MSG_MOTION            "CMD:MOTION\n"
#define EVT_MSG_NOT_WEARING       "CMD:NOT_WEARING\n"
#define EVT_MSG_START_SCREENING   "CMD:START_SCREENING\n"
#define ACK_WORKOUT_PAUSE         "ACK:WORKOUT_PAUSE\n"
#define ACK_WORKOUT_RESUME        "ACK:WORKOUT_RESUME\n"
#define ACK_WORKOUT_STOP          "ACK:WORKOUT_STOP\n"
#define ERR_WORKOUT_IN_PROGRESS   "ERR:WORKOUT_IN_PROGRESS\n"
#define ERR_WORKOUT_COOLDOWN      "ERR:WORKOUT_COOLDOWN\n"

// ---- Định dạng gói dữ liệu ----
// DATA:   millis,red,ir,bpm,spo2\n (gom 10 mẫu / gói) khi MEASURE / SCREENING
#define FMT_PPG_SAMPLE    "%lu,%lu,%lu,%u,%u\n"
// REPORT: trung bình pha 1 / các pha phụ của chu kỳ sàng lọc AFib
#define FMT_REPORT_PHASE1 "R1:%u,%u\n"
#define FMT_REPORT_PHASE2 "R2:%u,%u\n"
// REPORT: vitals khi tập (millis,bpm,steps)
#define FMT_WORKOUT       "W:%lu,%u,%lu\n"
// REPORT: mẫu tập luyện ghi đệm khi mất kết nối, gửi bù sau khi kết nối lại
#define FMT_RESYNC        "CMD:RESYNC_DATA,%lu,%u,%lu\n"
