#include "DisplayPower.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

// ====================
// Điều khiển màu LED trạng thái
// ====================
static Adafruit_NeoPixel* pixelsPtr = nullptr;

static void showColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!pixelsPtr) return;
  pixelsPtr->setPixelColor(0, pixelsPtr->Color(r, g, b));
  pixelsPtr->show();
}

// ====================
// Cấp nguồn MPU6050 và khởi tạo LED
// ====================
void DisplayPower_begin(uint8_t accelVddPin, uint8_t ledPin) {
  // Cấp nguồn cho MPU6050 trước khi khởi tạo cảm biến
  pinMode(accelVddPin, OUTPUT);
  digitalWrite(accelVddPin, HIGH);

  pixelsPtr = new Adafruit_NeoPixel(1, ledPin, NEO_GRB + NEO_KHZ800);
  pixelsPtr->begin();
  pixelsPtr->setBrightness(4);
  DisplayPower_showOn();
}

// ====================
// Hiển thị trạng thái bật, tắt và shutdown
// ====================
void DisplayPower_showOn() {
  showColor(0, 225, 0);
}

void DisplayPower_showRed() {
  showColor(225, 0, 0);
}

void DisplayPower_showOff() {
  if (!pixelsPtr) return;
  pixelsPtr->clear();
  pixelsPtr->show();
}
