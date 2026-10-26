// Cấu hình phần cứng và thông số vận hành dùng chung cho mọi module.
// Đổi chân / ngưỡng ở đây, không rải số trong từng file.
#pragma once

#include <Arduino.h>

// ====================
// Chân kết nối (Seeed XIAO ESP32-C3)
// ====================
#define BUTTON_PIN        D3   // Nút bấm (kéo lên nội, nhấn = LOW), cũng là chân đánh thức deep sleep
#define LED_PIN           D0   // LED NeoPixel báo trạng thái
#define ACCEL_VDD_PIN     D10  // Cấp nguồn MPU6050 (tắt khi deep sleep)
#define MPU6050_INT_PIN   D2   // Ngắt chuyển động của MPU6050
#define MAX30102_INT_PIN  D6   // Ngắt có dữ liệu của MAX30102
#define BATTERY_ADC_PIN   D1   // Điểm giữa cầu phân áp 2 x 100K đo điện áp pin
#define I2C_SDA_PIN       D4   // I2C dùng chung MPU6050 + MAX30102
#define I2C_SCL_PIN       D5

// ====================
// Thời gian (ms)
// ====================
#define CPU_FREQ_MHZ                 80        // 240MHz -> 80MHz để tiết kiệm pin
#define WAKE_HOLD_MS                 3000UL    // Giữ nút bao lâu để thức dậy từ deep sleep
#define LONG_PRESS_MS                4000UL    // Giữ nút bao lâu để tắt máy
#define LED_TIMEOUT_MS               3000UL    // LED tự tắt sau khi bấm nút
#define WEAR_CHECK_INTERVAL_MS       3000UL    // Chu kỳ dò đeo tay khi IDLE
#define WORKOUT_COOLDOWN_MS          600000UL  // 10 phút hồi phục tim sau tập: không tự đo AFib
#define WORKOUT_VITALS_INTERVAL_MS   3000UL    // Gom BPM + bước chân gửi mỗi 3 giây khi tập
#define BATTERY_UPDATE_INTERVAL_MS   30000UL

// ====================
// Ngưỡng
// ====================
#define WEAR_IR_THRESHOLD   50000L  // IR trên ngưỡng = đang đeo / có ngón tay
#define NO_FINGER_MS        2000UL  // IR dưới ngưỡng liên tục bao lâu thì coi là đã tháo

// Ngưỡng vung tay của MPU6050 theo chế độ (1-255, càng lớn càng khó báo chuyển động)
#define MOTION_THRESHOLD_IDLE       20
#define MOTION_THRESHOLD_MEASURE    5
#define MOTION_THRESHOLD_WORKOUT    20
#define MOTION_THRESHOLD_SCREENING  8

// ====================
// Phát hiện té ngã 3 giai đoạn (Free-fall -> Impact -> Immobility)
// Tương đương thuật toán trên XIAO-nRF52840
// ====================
#ifndef HS_FALL_TEST_ENABLED
#define HS_FALL_TEST_ENABLED 1
#endif
#define FALL_SAMPLE_MS              10UL     // 100Hz lấy mẫu gia tốc
#define FALL_FREE_FALL_G            0.35f    // Ngưỡng rơi tự do (< 0.35g)
#define FALL_FREE_FALL_MIN_SAMPLES  2        // Ít nhất 2 mẫu liên tiếp (~20ms) dưới ngưỡng
#define FALL_IMPACT_G               1.80f    // Ngưỡng va chạm (>= 1.8g)
#define FALL_IMPACT_TIMEOUT_MS      600UL    // Cửa sổ chờ va chạm sau cú rơi (600ms)
#define FALL_SETTLE_MS              500UL    // Thời gian rung chấn cơ học sau va chạm (500ms)
#define FALL_IMMOBILITY_WINDOW_MS   1500UL   // Cửa sổ kiểm tra bất động sau rung chấn (1500ms)
#define FALL_SAMPLE_INTERVAL_MS     50UL     // Chu kỳ kiểm tra cử động khi chờ bất động (50ms)
#define FALL_MOTION_DELTA_THRESH    1.50f    // Ngưỡng phát hiện cử động sau va chạm (~0.15g hay 1.5 m/s²)
#define FALL_MOTION_STREAK_THRESH   4        // Cần cử động liên tục >= 4 mẫu (~200ms) mới hủy báo động
#define FALL_DEBUG_INTERVAL_MS      500UL

// ====================
// Log Serial
// ====================
// Dòng CSV mỗi mẫu PPG (millis,ir,red,bpm,spo2,motion) cho scripts/log_to_csv.py
#ifndef HS_LOG_PPG_CSV
#if HS_FALL_TEST_ENABLED
#define HS_LOG_PPG_CSV 0  // Tránh CSV 100Hz che khuất log thử té ngã
#else
#define HS_LOG_PPG_CSV 1
#endif
#endif
// Dòng [ACCEL-DBG] mỗi giây
#ifndef HS_LOG_ACCEL_DEBUG
#define HS_LOG_ACCEL_DEBUG 1
#endif
