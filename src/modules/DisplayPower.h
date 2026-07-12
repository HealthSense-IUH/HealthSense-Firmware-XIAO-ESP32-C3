#ifndef DISPLAY_POWER_H
#define DISPLAY_POWER_H

#include <stdint.h>

void DisplayPower_begin(uint8_t accelVddPin, uint8_t ledPin);
void DisplayPower_showOn();
void DisplayPower_showOff();

#endif
