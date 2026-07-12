// Refactored main: delegates to module managers for BLE, PPG, accel, display/power
#include <Arduino.h>
#include "modules/BLEManager.h"
#include "modules/PPGManager.h"
#include "modules/AccelManager.h"
#include "modules/DisplayPower.h"
#include "modules/DeviceStateManager.h"

// >>> CẤU HÌNH PINOUT <<<
#define BUTTON_PIN D1
#define LED_PIN    D0
#define ACCEL_VDD  D10   // Nguồn MPU6050 nối chân D10

#define MPU6050_INT_PIN D2
const byte Max30102InterruptPin = D6;

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // wake-from-deep-sleep check (preserve original behavior)
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    long wakeStartTime = millis();
    bool heldLongEnough = true;
    while (millis() - wakeStartTime < 3000) {
      if (digitalRead(BUTTON_PIN) == HIGH) { heldLongEnough = false; break; }
      delay(10);
    }
    if (!heldLongEnough) {
      esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  }

  // power & display
  DisplayPower_begin(ACCEL_VDD, LED_PIN);

  // accel
  Serial.println("Dang khoi tao MPU6050...");
  if (!AccelManager_begin(MPU6050_INT_PIN)) {
    Serial.println("LOI: Khong tim thay MPU6050!");
  } else {
    Serial.println("MPU6050 OK!");
  }

  // BLE
  BLEManager_begin();

  // PPG sensor
  Serial.println("Dang khoi tao MAX30102...");
  if (!PPGManager_begin(Max30102InterruptPin)) {
    Serial.println("LOI: Khong tim thay MAX30102!");
    while (1);
  }
  Serial.println("MAX30102 OK!");

  DeviceStateManager_begin(BUTTON_PIN);
}

void loop() {
  DeviceStateManager_handleButton();

  // accel loop
  AccelManager_process();
  AccelManager_printDebug();

  if (AccelManager_popMotionEvent()) {
    DeviceStateManager_onEvent(EVT_MOTION);
  }

  // PPG processing (driven by interrupt)
  PPGManager_process();

  if (PPGManager_popNoFingerEvent()) {
    DeviceStateManager_onEvent(EVT_NOT_WEARING);
    Serial.println("[UNWEAR] Da thao dong ho. Tat LED tiet kiem pin!");
  }

  char packet[512];
  size_t len = 0;
  if (PPGManager_popPacket(packet, sizeof(packet), &len)) {
    if (BLEManager_isConnected()) {
      BLEManager_notify(packet, len);
    }
  }

  DeviceStateManager_loop();

  static unsigned long lastSerialPrintTime = 0;
  if (millis() - lastSerialPrintTime >= 1000) {
    lastSerialPrintTime = millis();
    // Serial.print("BPM: ");
    // Serial.print(PPGManager_getBPM());
    // Serial.print(" | SpO2: ");
    // Serial.println(PPGManager_getSpO2());
  }

  if (!BLEManager_isConnected()) delay(1);
}