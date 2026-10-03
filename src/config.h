// Cấu hình phần cứng và thông số vận hành dùng chung cho mọi module.
// Đổi chân / ngưỡng ở đây, không rải số trong từng file.
#pragma once

#include <Arduino.h>

// ---- Chân kết nối (Seeed XIAO ESP32-C3) ----
#define BUTTON_PIN        D3   // Nút bấm (kéo lên nội, nhấn = LOW), cũng là chân đánh thức deep sleep
#define LED_PIN           D0   // LED NeoPixel báo trạng thái
#define ACCEL_VDD_PIN     D10  // Cấp nguồn MPU6050 (tắt khi deep sleep)
#define MPU6050_INT_PIN   D2   // Ngắt chuyển động của MPU6050
#define MAX30102_INT_PIN  D6   // Ngắt có dữ liệu của MAX30102
#define BATTERY_ADC_PIN   D1   // Điểm giữa cầu phân áp 2 x 100K đo điện áp pin
#define I2C_SDA_PIN       D4   // I2C dùng chung MPU6050 + MAX30102
#define I2C_SCL_PIN       D5

// ---- Thời gian (ms) ----
#define CPU_FREQ_MHZ                 80        // 240MHz -> 80MHz để tiết kiệm pin
#define WAKE_HOLD_MS                 3000UL    // Giữ nút bao lâu để thức dậy từ deep sleep
#define LONG_PRESS_MS                4000UL    // Giữ nút bao lâu để tắt máy
#define LED_TIMEOUT_MS               3000UL    // LED tự tắt sau khi bấm nút
#define WEAR_CHECK_INTERVAL_MS       3000UL    // Chu kỳ dò đeo tay khi IDLE
#define WORKOUT_COOLDOWN_MS          600000UL  // 10 phút hồi phục tim sau tập: không tự đo AFib
#define WORKOUT_VITALS_INTERVAL_MS   3000UL    // Gom BPM + bước chân gửi mỗi 3 giây khi tập
#define BATTERY_UPDATE_INTERVAL_MS   30000UL

// ---- Ngưỡng ----
#define WEAR_IR_THRESHOLD   50000L  // IR trên ngưỡng = đang đeo / có ngón tay
#define NO_FINGER_MS        2000UL  // IR dưới ngưỡng liên tục bao lâu thì coi là đã tháo

// Ngưỡng vung tay của MPU6050 theo chế độ (1-255, càng lớn càng khó báo chuyển động)
#define MOTION_THRESHOLD_IDLE       20
#define MOTION_THRESHOLD_MEASURE    5
#define MOTION_THRESHOLD_WORKOUT    20
#define MOTION_THRESHOLD_SCREENING  8

// ---- Log Serial ----
// Dòng CSV mỗi mẫu PPG (millis,ir,red,bpm,spo2,motion) cho scripts/log_to_csv.py
#ifndef HS_LOG_PPG_CSV
#define HS_LOG_PPG_CSV 1
#endif
// Dòng [ACCEL-DBG] mỗi giây
#ifndef HS_LOG_ACCEL_DEBUG
#define HS_LOG_ACCEL_DEBUG 1
#endif
