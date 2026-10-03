#include "AccelManager.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include "../config.h"

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

// ISR phải nằm trong IRAM trên ESP32 và chạy thật nhanh: chỉ bật cờ
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
  // Không dùng con quay và cảm biến nhiệt: cho ngủ để tiết kiệm pin
  mpu.setGyroStandby(true, true, true);
  mpu.setTemperatureStandby(true);

  // Chân INT: chốt mức, active-high, báo khi có chuyển động
  mpu.setInterruptPinLatch(true);
  mpu.setInterruptPinPolarity(false);
  mpu.setMotionInterrupt(true);

  pinMode(intPin, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(intPin), mpuInterruptHandler, RISING);

  Serial.println("MPU6050 OK và đã bật Ngắt chuyển động!");
  return true;
}

bool AccelManager_isReady() { return mpuReady; }
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

void AccelManager_resetStepCount() {
  stepCount = 0;
  lastStepTime = 0;
}

void AccelManager_updatePedometer() {
  if (!mpuReady) return;

  static unsigned long lastSampleTime = 0;
  unsigned long now = millis();
  if (now - lastSampleTime < PEDOMETER_SAMPLE_MS) return;
  lastSampleTime = now;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float mag = sqrt(a.acceleration.x * a.acceleration.x +
                   a.acceleration.y * a.acceleration.y +
                   a.acceleration.z * a.acceleration.z);

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

void AccelManager_process() {
  AccelManager_updatePedometer();
}

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
