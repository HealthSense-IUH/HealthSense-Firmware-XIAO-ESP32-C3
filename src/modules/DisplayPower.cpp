#include "DisplayPower.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

static Adafruit_NeoPixel* pixelsPtr = nullptr;

void DisplayPower_begin(uint8_t accelVddPin, uint8_t ledPin) {
  pinMode(accelVddPin, OUTPUT);
  digitalWrite(accelVddPin, HIGH);

  pixelsPtr = new Adafruit_NeoPixel(1, ledPin, NEO_GRB + NEO_KHZ800);
  pixelsPtr->begin();
  pixelsPtr->setBrightness(4);
  pixelsPtr->setPixelColor(0, pixelsPtr->Color(0,225,0));
  pixelsPtr->show();
}

void DisplayPower_showOn() {
  if (!pixelsPtr) return;
  pixelsPtr->setPixelColor(0, pixelsPtr->Color(0,225,0));
  pixelsPtr->show();
}

void DisplayPower_showRed() {
  if (!pixelsPtr) return;
  pixelsPtr->setPixelColor(0, pixelsPtr->Color(225,0,0));
  pixelsPtr->show();
}

void DisplayPower_showOff() {
  if (!pixelsPtr) return;
  pixelsPtr->clear();
  pixelsPtr->show();
}
