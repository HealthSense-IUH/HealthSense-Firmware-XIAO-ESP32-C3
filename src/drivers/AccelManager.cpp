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
// Trạng thái phát hiện té ngã thử nghiệm
// ====================
#if HS_FALL_TEST_ENABLED
enum FallTestState : uint8_t { FALL_IDLE, FALL_WAIT_ROTATION, FALL_WAIT_STILL, FALL_COOLDOWN };
static FallTestState fallState = FALL_IDLE;
static unsigned long fallCandidateTime = 0;
static unsigned long lastRotationTime = 0;
static bool hasRecentRotation = false;
static unsigned long stillStartTime = 0;
static bool trackingStillness = false;
static unsigned long lastFallSampleTime = 0;
static bool hasFallSample = false;
static unsigned long fallCooldownTime = 0;
static float lastFallAccelG = 1.0f;
static float lastFallRotationDps = 0.0f;
static bool wasAboveImpact = false;
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

  // Lọc thông cao: bỏ thành phần trọng lực, chỉ giữ cử động
  mpu.setHighPassFilter(MPU6050_HIGHPASS_0_63_HZ);
  // Ngưỡng (1-255) và thời gian tối thiểu (ms) của một lần vung tay; ngưỡng đổi theo chế độ qua setMotionThreshold
  mpu.setMotionDetectionThreshold(MOTION_THRESHOLD_MEASURE);
  mpu.setMotionDetectionDuration(20);
  // ±4g: đủ nhạy cho đi bộ / chạy / vung tay mà không bị chạm trần như ±2g
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  // Lọc thông thấp phần cứng 21Hz
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  // Bật con quay chỉ khi thử té ngã; cảm biến nhiệt vẫn ngủ.
#if HS_FALL_TEST_ENABLED
  mpu.setGyroStandby(false, false, false);
  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
#else
  mpu.setGyroStandby(true, true, true);
#endif
  mpu.setTemperatureStandby(true);

  // Chân INT: chốt mức, active-high, báo khi có chuyển động
  mpu.setInterruptPinLatch(true);
  mpu.setInterruptPinPolarity(false);
  mpu.setMotionInterrupt(true);

  pinMode(intPin, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(intPin), mpuInterruptHandler, RISING);

  Serial.println("MPU6050 OK và đã bật Ngắt chuyển động!");
#if HS_FALL_TEST_ENABLED
  Serial.printf("[FALL] TEST ENABLED: impact >= %.2fg + rotation >= %.1fdeg/s + still %lums. Serial only.\n",
                FALL_IMPACT_G, FALL_ROTATION_DPS, FALL_STILL_MS);
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
// Chuỗi va chạm, xoay nhanh và ít chuyển động
// ====================
#if HS_FALL_TEST_ENABLED
/** Ngưỡng thử nghiệm trên cổ tay; không bắt buộc pha rơi tự do, không kết luận chẩn đoán. */
static void updateFallDetection(float accelG, float rotationDps, unsigned long now) {
  lastFallAccelG = accelG;
  lastFallRotationDps = rotationDps;
  bool sampleGap = hasFallSample && now - lastFallSampleTime > FALL_MAX_SAMPLE_GAP_MS;
  lastFallSampleTime = now;
  hasFallSample = true;
  if (sampleGap || !isfinite(accelG) || !isfinite(rotationDps)) {
    trackingStillness = false;
    hasRecentRotation = false;
  }
  if (!isfinite(accelG) || !isfinite(rotationDps)) return;

  bool impact = accelG >= FALL_IMPACT_G;
  bool impactRising = impact && !wasAboveImpact;
  wasAboveImpact = impact;
  if (rotationDps >= FALL_ROTATION_DPS) {
    lastRotationTime = now;
    hasRecentRotation = true;
  }

  if (fallState == FALL_COOLDOWN) {
    if (now - fallCooldownTime >= FALL_COOLDOWN_MS) {
      fallState = FALL_IDLE;
      Serial.println("[FALL] READY");
    }
    return;
  }

  if (fallState == FALL_IDLE) {
    if (!impactRising) return;
    fallCandidateTime = now;
    trackingStillness = false;
    bool rotated = hasRecentRotation && now - lastRotationTime <= FALL_ROTATION_BEFORE_MS;
    fallState = rotated ? FALL_WAIT_STILL : FALL_WAIT_ROTATION;
    Serial.printf("[FALL] IMPACT: A=%.2fg W=%.1fdeg/s; %s\n", accelG, rotationDps,
                  rotated ? "rotation found, waiting for stillness" : "waiting for rotation");
    return;
  }

  if (fallState == FALL_WAIT_ROTATION) {
    if (now - fallCandidateTime > FALL_ROTATION_AFTER_MS) {
      fallState = FALL_IDLE;
      Serial.println("[FALL] REJECTED: no fast rotation near impact");
      return;
    }
    if (rotationDps >= FALL_ROTATION_DPS) {
      fallState = FALL_WAIT_STILL;
      Serial.printf("[FALL] ROTATION: waiting for %lums of stillness\n", FALL_STILL_MS);
    }
    return;
  }

  if (now - fallCandidateTime > FALL_CONFIRM_TIMEOUT_MS) {
    fallState = FALL_IDLE;
    trackingStillness = false;
    Serial.println("[FALL] REJECTED: no continuous stillness before timeout");
    return;
  }
  bool still = fabsf(accelG - 1.0f) <= FALL_STILL_G_TOLERANCE && rotationDps <= FALL_STILL_DPS;
  if (!still) {
    trackingStillness = false;
    return;
  }
  if (!trackingStillness) {
    trackingStillness = true;
    stillStartTime = now;
  }
  if (now - stillStartTime >= FALL_STILL_MS) {
    Serial.println("[FALL] SUSPECTED: impact + rotation + stillness. Experimental, Serial only.");
    fallState = FALL_COOLDOWN;
    fallCooldownTime = now;
    trackingStillness = false;
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
#if HS_FALL_TEST_ENABLED
  // Adafruit trả gia tốc m/s² và tốc độ xoay rad/s.
  float rotationDps = sqrtf(g.gyro.x * g.gyro.x + g.gyro.y * g.gyro.y + g.gyro.z * g.gyro.z) * RAD_TO_DEG;
  updateFallDetection(mag / 9.80665f, rotationDps, now);
  static unsigned long lastFallDebugTime = 0;
  if (now - lastFallDebugTime >= FALL_DEBUG_INTERVAL_MS) {
    lastFallDebugTime = now;
    const char* stateNames[] = {"IDLE", "WAIT_ROTATION", "WAIT_STILL", "COOLDOWN"};
    Serial.printf("[FALL-DATA] A=%.2fg W=%.1fdeg/s state=%s\n",
                  lastFallAccelG, lastFallRotationDps, stateNames[fallState]);
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
