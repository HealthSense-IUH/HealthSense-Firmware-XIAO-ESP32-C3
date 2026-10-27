#include "AccelManager.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include "config/config.h"

// ====================
// Trạng thái MPU6050 và thông số đếm bước
// ====================
static Adafruit_MPU6050 mpu;
static bool mpuReady = false;
static volatile bool isMoving = false;
static uint8_t motionStatus;

// Đếm bước (peak detection trên độ lớn gia tốc)
#define PEDOMETER_SAMPLE_MS   40      // 25Hz
#define STEP_PEAK_MS2         12.2f   // ~2,4 m/s² trên trọng lực 9,8 m/s²
#define STEP_MIN_INTERVAL_MS  280
#define STEP_MAX_INTERVAL_MS  1200
static uint32_t stepCount = 0;
static unsigned long lastStepTime = 0;
static float prevAccelMag = 9.81f;

// ====================
// Trạng thái phát hiện té ngã 3 giai đoạn (Chuẩn logic từ XIAO-nRF52840)
// Giai đoạn 1: Free-fall (Rơi tự do: gia tốc tổng < 0.35g trong >= 20ms)
// Giai đoạn 2: Impact (Va chạm: gia tốc tổng >= 1.8g trong vòng 600ms sau rơi)
// Giai đoạn 3: Immobility (Bất động tối thiểu 1.5s sau va chạm; loại bỏ báo động giả khi vung tay/chạy bộ)
// ====================
#if HS_FALL_TEST_ENABLED
enum FallCheckState : uint8_t {
  FALL_STATE_IDLE = 0,
  FALL_STATE_WAIT_IMPACT,
  FALL_STATE_WAIT_IMMOBILITY
};

static FallCheckState fallState = FALL_STATE_IDLE;
static unsigned long fallStateStartMs = 0;
static unsigned long impactDetectedAt = 0;
static unsigned long fallLastSampleMs = 0;
static float fallPrevAx = 0.0f;
static float fallPrevAy = 0.0f;
static float fallPrevAz = 0.0f;
static uint8_t freeFallSampleCount = 0;
static volatile bool fallEventFlag = false;
static bool fallDetectionEnabled = true;
static uint8_t fallMotionStreak = 0;
#endif

// ISR phải nằm trong IRAM trên ESP32 và chạy thật nhanh: chỉ bật cờ
// ====================
// Ngắt chuyển động và khởi tạo cảm biến
// ====================
static void IRAM_ATTR mpuInterruptHandler() {
  isMoving = true;
}

bool AccelManager_begin(uint8_t intPin) {
  if (!mpu.begin()) {
    mpuReady = false;
    return false;
  }
  mpuReady = true;

  // Tắt lọc thông cao để giữ nguyên thành phần trọng lực 1g (cần thiết cho phát hiện rơi tự do 0g)
  mpu.setHighPassFilter(MPU6050_HIGHPASS_DISABLE);
  // Ngưỡng (1-255) và thời gian tối thiểu (ms) của một lần vung tay; ngưỡng đổi theo chế độ qua setMotionThreshold
  mpu.setMotionDetectionThreshold(MOTION_THRESHOLD_MEASURE);
  mpu.setMotionDetectionDuration(20);
  // ±4g: đủ nhạy cho đi bộ / chạy / vung tay mà không bị chạm trần như ±2g
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  // Lọc thông thấp phần cứng 21Hz
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  // Không dùng con quay và cảm biến nhiệt: cho ngủ để tiết kiệm pin tối đa
  mpu.setGyroStandby(true, true, true);
  mpu.setTemperatureStandby(true);

  // Chân INT: chốt mức, active-high, báo khi có chuyển động
  mpu.setInterruptPinLatch(true);
  mpu.setInterruptPinPolarity(false);
  mpu.setMotionInterrupt(true);

  pinMode(intPin, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(intPin), mpuInterruptHandler, RISING);

  Serial.println("MPU6050 OK và đã bật Ngắt chuyển động!");
#if HS_FALL_TEST_ENABLED
  Serial.printf("[FALL] 3-STAGE FALL DETECTION ARMED: FreeFall < %.2fg -> Impact >= %.2fg (600ms) -> Stillness %lums\n",
                FALL_FREE_FALL_G, FALL_IMPACT_G, FALL_IMMOBILITY_WINDOW_MS);
#endif
  return true;
}

// ====================
// Truy xuất chuyển động và số bước
// ====================
bool AccelManager_isMoving() { return isMoving; }

bool AccelManager_popMotionEvent() {
  bool motionEvent = false;
  noInterrupts();
  if (isMoving) {
    motionEvent = true;
    isMoving = false;
  }
  interrupts();
  // Đọc thanh ghi trạng thái để nhả chân INT đang chốt
  if (motionEvent) motionStatus = mpu.getMotionInterruptStatus();
  return motionEvent;
}

uint32_t AccelManager_getStepCount() {
  return stepCount;
}

// ====================
// Truy xuất sự kiện và điều khiển phát hiện té ngã
// ====================
bool AccelManager_popFallEvent() {
#if HS_FALL_TEST_ENABLED
  bool fallEvent = false;
  noInterrupts();
  if (fallEventFlag) {
    fallEvent = true;
    fallEventFlag = false;
  }
  interrupts();
  return fallEvent;
#else
  return false;
#endif
}

void AccelManager_startFallDetection() {
#if HS_FALL_TEST_ENABLED
  fallDetectionEnabled = true;
  fallState = FALL_STATE_IDLE;
  freeFallSampleCount = 0;
  Serial.println("[ACCEL] Fall detection armed");
#endif
}

void AccelManager_stopFallDetection() {
#if HS_FALL_TEST_ENABLED
  fallDetectionEnabled = false;
  fallState = FALL_STATE_IDLE;
  freeFallSampleCount = 0;
#endif
}

bool AccelManager_isFallDetectionEnabled() {
#if HS_FALL_TEST_ENABLED
  return fallDetectionEnabled;
#else
  return false;
#endif
}

/** Lấy mẫu gia tốc 25Hz và đếm bước. */
// ====================
// Tính độ lớn gia tốc và đếm bước chân
// ====================
static void updatePedometer(float mag, unsigned long now) {
  // Một bước = vượt ngưỡng đi lên, cách bước trước 280-1200ms (bước đầu tiên luôn tính)
  if (mag > STEP_PEAK_MS2 && prevAccelMag <= STEP_PEAK_MS2) {
    unsigned long sinceLastStep = now - lastStepTime;
    if (lastStepTime == 0 || (sinceLastStep >= STEP_MIN_INTERVAL_MS && sinceLastStep <= STEP_MAX_INTERVAL_MS)) {
      stepCount++;
      lastStepTime = now;
    }
  }
  prevAccelMag = mag;
}

// ====================
// Thuật toán phát hiện té ngã 3 giai đoạn (Free-fall -> Impact -> Immobility)
// Đồng bộ logic chuẩn từ XIAO-nRF52840
// ====================
#if HS_FALL_TEST_ENABLED
static void updateFallDetection(const sensors_event_t &a, float gMag, unsigned long now) {
  if (!fallDetectionEnabled) {
    fallState = FALL_STATE_IDLE;
    freeFallSampleCount = 0;
    return;
  }

  // --- GIAI ĐOẠN 1: Phát hiện rơi tự do (Free-fall) ---
  // Khi rơi tự do, trọng lượng biểu kiến giảm mạnh (gMag < 0.35g liên tục >= 2 mẫu / 20ms)
  if (gMag <= FALL_FREE_FALL_G) {
    freeFallSampleCount++;
    if (freeFallSampleCount >= FALL_FREE_FALL_MIN_SAMPLES) {
      if (fallState == FALL_STATE_IDLE || fallState == FALL_STATE_WAIT_IMPACT) {
        fallState = FALL_STATE_WAIT_IMPACT;
        fallStateStartMs = now;
        Serial.println("[ACCEL] Giai doan 1: Free-fall detected -> Cho va cham (WAIT_IMPACT)");
      }
    }
  } else {
    freeFallSampleCount = 0;
  }

  if (fallState == FALL_STATE_IDLE) {
    return;
  }

  // --- GIAI ĐOẠN 2: Chờ va chạm mặt sàn (Impact Detection trong 600ms) ---
  if (fallState == FALL_STATE_WAIT_IMPACT) {
    // Quá thời gian 600ms mà không có va chạm -> Cú rơi giả / an toàn
    if (now - fallStateStartMs > FALL_IMPACT_TIMEOUT_MS) {
      fallState = FALL_STATE_IDLE;
      fallMotionStreak = 0;
      Serial.println("[ACCEL] Giai doan 2: Khong co va cham sau 600ms -> Bo qua");
    } else if (gMag >= FALL_IMPACT_G) {
      // Va chạm: gia tốc tổng >= 1.8g
      fallState = FALL_STATE_WAIT_IMMOBILITY;
      impactDetectedAt = now;
      fallLastSampleMs = now;
      fallPrevAx = a.acceleration.x;
      fallPrevAy = a.acceleration.y;
      fallPrevAz = a.acceleration.z;
      fallMotionStreak = 0;
      Serial.printf("[ACCEL] Giai doan 2: Phat hien va cham (gMag=%.2f) -> Cho bat dong (WAIT_IMMOBILITY)\n", gMag);
    }
    return;
  }

  // --- GIAI ĐOẠN 3: Chờ bất động (Immobility Window) ---
  if (fallState == FALL_STATE_WAIT_IMMOBILITY) {
    unsigned long timeSinceImpact = now - impactDetectedAt;

    // Trong FALL_SETTLE_MS (500ms) đầu tiên sau va chạm: giai đoạn nảy đập cơ học / hấp thụ xung lực,
    // liên tục cập nhật mẫu gia tốc baseline mà không bắt lỗi cử động
    if (timeSinceImpact < FALL_SETTLE_MS) {
      fallPrevAx = a.acceleration.x;
      fallPrevAy = a.acceleration.y;
      fallPrevAz = a.acceleration.z;
      fallLastSampleMs = now;
      fallMotionStreak = 0;
    } else {
      // Sau thời gian rung chấn: kiểm tra cử động định kỳ mỗi 50ms
      if (now - fallLastSampleMs >= FALL_SAMPLE_INTERVAL_MS) {
        fallLastSampleMs = now;
        float delta = fabsf(a.acceleration.x - fallPrevAx) +
                      fabsf(a.acceleration.y - fallPrevAy) +
                      fabsf(a.acceleration.z - fallPrevAz);
        fallPrevAx = a.acceleration.x;
        fallPrevAy = a.acceleration.y;
        fallPrevAz = a.acceleration.z;

        if (delta > FALL_MOTION_DELTA_THRESH) {
          fallMotionStreak++;
          Serial.printf("[ACCEL] Giai doan 3: Cu dong nhe (delta=%.2f m/s2, streak=%u/%u)\n",
                        delta, fallMotionStreak, FALL_MOTION_STREAK_THRESH);

          // Chỉ khi cử động liên tục >= 4 mẫu (~200ms) chứng tỏ người dùng đang hoạt động bình thường / đứng dậy
          if (fallMotionStreak >= FALL_MOTION_STREAK_THRESH) {
            fallState = FALL_STATE_IDLE;
            fallMotionStreak = 0;
            Serial.printf("[ACCEL] Giai doan 3: Phat hien hoat dong lien tuc (streak>=%u) -> Huy bao dong (Nguoi dung van hoat dong)\n", FALL_MOTION_STREAK_THRESH);
            return;
          }
        } else {
          // Lắng xuống: giảm streak dần
          if (fallMotionStreak > 0) fallMotionStreak--;
        }
      }

      // Nếu đã qua đủ thời gian kiểm tra bất động (FALL_SETTLE_MS + FALL_IMMOBILITY_WINDOW_MS)
      // mà không bị hủy bởi chuỗi cử động liên tục -> Xác nhận té ngã thật sự!
      if (timeSinceImpact >= (FALL_SETTLE_MS + FALL_IMMOBILITY_WINDOW_MS)) {
        fallEventFlag = true;
        fallState = FALL_STATE_IDLE;
        fallMotionStreak = 0;
        Serial.println("[ACCEL] Giai doan 3: Xac nhan bat dong -> TE NGA XAC NHAN! (fallEventFlag=1)");
      }
    }
  }
}
#endif

// ====================
// Lấy mẫu chung cho đếm bước và thử té ngã
// ====================
void AccelManager_process() {
  if (!mpuReady) return;
  static unsigned long lastSensorSampleTime = 0;
  static unsigned long lastPedometerSampleTime = 0;
  unsigned long now = millis();
#if HS_FALL_TEST_ENABLED
  if (now - lastSensorSampleTime < FALL_SAMPLE_MS) return;
#else
  if (now - lastSensorSampleTime < PEDOMETER_SAMPLE_MS) return;
#endif
  lastSensorSampleTime = now;
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float mag = sqrtf(a.acceleration.x * a.acceleration.x +
                    a.acceleration.y * a.acceleration.y +
                    a.acceleration.z * a.acceleration.z);
  if (now - lastPedometerSampleTime >= PEDOMETER_SAMPLE_MS) {
    lastPedometerSampleTime = now;
    updatePedometer(mag, now);
  }

  // Phát hiện cử động cánh tay qua gia tốc động lực (software fallback khi chân INT phần cứng chưa kích hoạt)
  float dynamicAccel = fabsf(mag - 9.80665f);
  if (dynamicAccel > 2.2f) { // Chuyển động rung lắc / vung tay > ~0.22g
    isMoving = true;
  }
#if HS_FALL_TEST_ENABLED
  float gMag = mag / 9.80665f;
  updateFallDetection(a, gMag, now);
  static unsigned long lastFallDebugTime = 0;
  if (now - lastFallDebugTime >= FALL_DEBUG_INTERVAL_MS) {
    lastFallDebugTime = now;
    if (fallState != FALL_STATE_IDLE) {
      const char* stateNames[] = {"IDLE", "WAIT_IMPACT", "WAIT_IMMOBILITY"};
      Serial.printf("[FALL-DATA] A=%.2fg state=%s\n", gMag, stateNames[fallState]);
    }
  }
#endif
}

// ====================
// Log chẩn đoán và ngưỡng chuyển động
// ====================
void AccelManager_printDebug() {
#if HS_LOG_ACCEL_DEBUG
  static unsigned long lastDebugPrint = 0;
  if (!mpuReady || millis() - lastDebugPrint < 1000) return;
  lastDebugPrint = millis();

  Serial.print("[");
  Serial.print(millis());
  Serial.print("] [ACCEL-DBG] isMoving=");
  Serial.print(isMoving ? "1" : "0");
  Serial.print(" | steps=");
  Serial.print(stepCount);
  Serial.print(" | motionStatus=");
  Serial.println(motionStatus);
#endif
}

void AccelManager_setMotionThreshold(uint8_t threshold) {
  if (!mpuReady) return;
  mpu.setMotionDetectionThreshold(threshold);
  Serial.print("[ACCEL] Đã đổi ngưỡng vung tay thành: ");
  Serial.println(threshold);
}
