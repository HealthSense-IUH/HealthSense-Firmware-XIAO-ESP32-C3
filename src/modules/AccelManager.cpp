#include "AccelManager.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>

static Adafruit_MPU6050 mpu;
static bool mpuReady = false;
volatile bool isMoving = false;
static volatile uint32_t motionIsrCount = 0;
static uint8_t motionStatus;

// Hàm Ngắt ISR (Bắt buộc phải có chữ IRAM_ATTR trên ESP32)
// Hàm này chạy cực nhanh, chớp nhoáng khi chân INT có tín hiệu
void IRAM_ATTR mpuInterruptHandler() {
  isMoving = true; 
  motionIsrCount++;
}

bool AccelManager_begin(uint8_t intPin) {
  if (!mpu.begin()) {
    mpuReady = false;
    return false;
  }
  mpuReady = true;

  //Thiết lập bộ lộc thông cao (High Pass Filter) để bỏ qua trọng lực Trái Đất
  mpu.setHighPassFilter(MPU6050_HIGHPASS_0_63_HZ);

  //Thiết lập ngưỡng phát hiện chuyển động (1-255)
  //Số càng to, vung tay càng mạnh thì cảm biến mới báo cáo về mạch
  mpu.setMotionDetectionThreshold(5);

  //Thiết lập thời gian chuyển động tối thiểu (1-255ms) để loại bỏ nhiễu rung lắc nhẹ
  mpu.setMotionDetectionDuration(20);

  /**
   * Thiết lập giới hạn tầm đo của cảm biến gia tốc
   * Cảm biến MPU6050 có các dải đo từ ±2g, ±4g, ±8g đến ±16g.
   * Nếu để ±2g: Quá nhạy, vung tay nhẹ là thông số bị chạm trần (clipping).
   * Nếu để ±16g: Dùng để đo va chạm mạnh (như tai nạn ô tô), dùng cho cổ tay sẽ bị kém nhạy, khó nhận biết cử động.
   * 
   * Mức ±4g là "điểm ngọt" (sweet spot) hoàn hảo nhất để theo dõi chuyển động sinh lý học của con người (đi bộ, chạy bộ, vung tay)
   */
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
  //Bật Bộ lọc thông thấp kỹ thuật số (DLPF - Digital Low Pass Filter) tích hợp sẵn trong phần cứng MPU6050 ở tần số cắt 21Hz.
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  //Đưa cả 3 trục X, Y, Z của cảm biến con quay hồi chuyển (Gyroscope) vào chế độ ngủ sâu vì không cần Gyro.
  mpu.setGyroStandby(true, true, true);
  //Tắt cảm biến nhiệt độ bên trong MPU6050 vi không cần thiết.
  mpu.setTemperatureStandby(true);

  //Kích hoạt chân ngắt (Interuption - INT)
  mpu.setInterruptPinLatch(true);
  mpu.setInterruptPinPolarity(false);
  mpu.setMotionInterrupt(true);

  // Kích hoạt ngắt trên ESP32
  pinMode(intPin, INPUT_PULLDOWN);
  // FALLING, RISING hoặc CHANGE tùy thuộc vào Polarity ở trên (Mặc định RISING là từ 0V lên 3.3V)
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
  if (motionEvent) {
    motionStatus = mpu.getMotionInterruptStatus();
  }
  return motionEvent;
}

void AccelManager_process() {
  if (!mpuReady) return;
}

void AccelManager_printDebug() {
  static unsigned long lastDebugPrint = 0;
  if (!mpuReady) return;

  if (millis() - lastDebugPrint >= 1000) {
    lastDebugPrint = millis();

    Serial.print("[");
    Serial.print(millis());
    Serial.print("] [ACCEL-DBG] isMoving=");
    Serial.print(isMoving ? "1" : "0");
    Serial.print(" | isrCount=");
    Serial.print((uint32_t)motionIsrCount);
    Serial.print(" | motionStatus=");
    Serial.println(motionStatus);
  }
}

void AccelManager_setMotionThreshold(uint8_t threshold) {
  if (!mpuReady) return;
  mpu.setMotionDetectionThreshold(threshold);
  Serial.print("[ACCEL] Đã đổi ngưỡng vung tay thành: ");
  Serial.println(threshold);
}
