// HealthSense firmware (XIAO ESP32-C3): main chỉ khởi tạo module và điều phối luồng dữ liệu theo chế độ.
// Tầng: config/ (chân, ngưỡng, giao thức BLE) <- drivers/ (cảm biến, LED) <- comm/ (BLE) <- app/ (chế độ, sàng lọc AFib).
#include <Arduino.h>
#include "config/config.h"
#include "config/BleProtocol.h"
#include "drivers/AccelManager.h"
#include "drivers/DisplayPower.h"
#include "drivers/PPGManager.h"
#include "comm/BLEManager.h"
#include "app/DeviceStateManager.h"

static void goToDeepSleep() {
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

/** Thức dậy từ deep sleep do nút: phải giữ đủ WAKE_HOLD_MS, nhả sớm thì ngủ tiếp (tránh bật nhầm). */
static void requireLongPressToWake() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) return;
  unsigned long wakeStartTime = millis();
  while (millis() - wakeStartTime < WAKE_HOLD_MS) {
    if (digitalRead(BUTTON_PIN) == HIGH) goToDeepSleep();
    delay(10);
  }
}

void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  requireLongPressToWake();

  DisplayPower_begin(ACCEL_VDD_PIN, LED_PIN);

  Serial.println("Dang khoi tao MPU6050...");
  Serial.println(AccelManager_begin(MPU6050_INT_PIN) ? "MPU6050 OK!" : "LOI: Khong tim thay MPU6050!");

  BLEManager_begin();

  Serial.println("Dang khoi tao MAX30102...");
  if (!PPGManager_begin(MAX30102_INT_PIN)) {
    Serial.println("LOI: Khong tim thay MAX30102!");
    while (1);
  }
  Serial.println("MAX30102 OK!");

  DeviceStateManager_begin(BUTTON_PIN, ACCEL_VDD_PIN);
}

/** WORKOUT: gom BPM + bước chân gửi mỗi 3 giây (mất kết nối thì ghi đệm), không gửi PPG thô. */
static void streamWorkout() {
  static unsigned long lastVitalsSend = 0;
  if (millis() - lastVitalsSend >= WORKOUT_VITALS_INTERVAL_MS) {
    lastVitalsSend = millis();
    uint8_t bpm = PPGManager_getBPM();
    uint32_t steps = AccelManager_getStepCount();

    if (BLEManager_isConnected()) {
      char buf[64];
      int len = snprintf(buf, sizeof(buf), FMT_WORKOUT, (unsigned long)millis(), (unsigned int)bpm,
                         (unsigned long)steps);
      BLEManager_notifyReport(buf, len);
    } else {
      BLEManager_pushOfflineSample(bpm, steps);
    }
  }
  PPGManager_discardPacket();
}

/** MEASURE / SCREENING: gửi PPG thô theo gói 10 mẫu (millis,red,ir,bpm,spo2). */
static void streamPPG() {
  char packet[512];
  size_t len = 0;
  if (PPGManager_popPacket(packet, sizeof(packet), &len) && BLEManager_isConnected()) {
    BLEManager_notify(packet, len);
  }
}

void loop() {
  DeviceStateManager_handleButton();

  AccelManager_process();
  AccelManager_printDebug();
  if (AccelManager_popMotionEvent()) DeviceStateManager_onEvent(EVT_MOTION);

  PPGManager_process();
  if (PPGManager_popNoFingerEvent()) {
    DeviceStateManager_onEvent(EVT_NOT_WEARING);
    Serial.println("[UNWEAR] Da thao dong ho. Tat LED tiet kiem pin!");
  }

  switch (DeviceStateManager_getMode()) {
    case MODE_WORKOUT:
      streamWorkout();
      break;
    case MODE_MEASURE:
    case MODE_SCREENING:
      streamPPG();
      break;
    default:
      break;  // IDLE / SHUTDOWN / TURN_ON: không gửi dữ liệu
  }

  DeviceStateManager_loop();
  BLEManager_loop();

  static unsigned long lastBatteryUpdateTime = 0;
  if (millis() - lastBatteryUpdateTime >= BATTERY_UPDATE_INTERVAL_MS) {
    lastBatteryUpdateTime = millis();
    BLEManager_updateBatteryLevel();
  }

  if (!BLEManager_isConnected()) delay(1);
}
