#include "PPGManager.h"
#include <MAX30105.h>
#include <heartRate.h>
#include <Arduino.h>
#include "AccelManager.h"

static MAX30105 particleSensor;
static const byte* interruptPinPtr = nullptr;
static volatile bool dataReady = false;

#define PPG_PACKET_SIZE_LOCAL 10
static char ppgPayload[512] = "";
static uint8_t ppgSampleCount = 0;

// buffers for SpO2 calculation
#define BUFFER_SIZE_LOCAL 167
static uint32_t redBuffer[BUFFER_SIZE_LOCAL];
static uint32_t irBuffer[BUFFER_SIZE_LOCAL];
static int bufferIndex = 0;
static bool bufferFull = false;

static uint8_t finalBPM = 0;
static uint8_t finalSpO2 = 0;
static long lastBeatTime = 0;
static float beatAvg = 0;

// no-finger detection (mirror original behavior)
static unsigned long noFingerStartTime = 0;
static volatile bool noFingerEventFlag = false;

void IRAM_ATTR PPGManager_handleInterrupt() {
  dataReady = true;
}

bool PPGManager_begin(uint8_t interruptPin) {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    return false;
  }

  Wire.setTimeOut(25);
  // particleSensor.setup(30, 4, 2, 400, 411, 16384);
  particleSensor.setup(30, 4, 2, 400, 215, 16384);
  // particleSensor.setPulseAmplitudeRed(15);
  // particleSensor.setPulseAmplitudeIR(60);
  // particleSensor.setPulseAmplitudeRed(127);
  // particleSensor.setPulseAmplitudeIR(127);
  particleSensor.setPulseAmplitudeRed(80); 
  particleSensor.setPulseAmplitudeIR(80);
  // particleSensor.setPulseAmplitudeRed(60); 
  // particleSensor.setPulseAmplitudeIR(60);

  pinMode(interruptPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(interruptPin), PPGManager_handleInterrupt, FALLING);
  particleSensor.enableDATARDY();

  delay(10);
  particleSensor.getINT1();
  particleSensor.getINT2();
  dataReady = false;

  return true;
}

void PPGManager_wakeUp() {
  particleSensor.wakeUp();
  particleSensor.clearFIFO();
}

void PPGManager_shutDown() {
  particleSensor.shutDown();
}

static uint8_t filter3(uint8_t newVal) {
  static uint8_t b[3] = {0,0,0};
  b[0] = b[1]; b[1] = b[2]; b[2] = newVal;
  uint8_t x=b[0], y=b[1], z=b[2];
  if ((x - y) * (z - x) >= 0) return x;
  if ((y - x) * (z - y) >= 0) return y;
  return z;
}

void PPGManager_process() {
  if (!dataReady) return;
  dataReady = false;

  particleSensor.getINT1();
  particleSensor.getINT2();
  particleSensor.check();

  while (particleSensor.available()) {
    long irValue = particleSensor.getFIFOIR();
    long redValue = particleSensor.getFIFORed();

    if (irValue < 50000) {
      if (noFingerStartTime == 0) {
        noFingerStartTime = millis();
      }
      if (millis() - noFingerStartTime > 2000) {
        // reset internal PPG state
        finalBPM = 0;
        finalSpO2 = 0;
        beatAvg = 0;
        bufferIndex = 0;
        bufferFull = false;
        noFingerEventFlag = true;
        noFingerStartTime = 0;
        // leave shutdown to caller
        break; // stop processing FIFO
      }
    } else {
      noFingerStartTime = 0;
      // package for BLE
      char tempStr[64];
      sprintf(tempStr, "%lu,%u,%u\n", millis(), (uint32_t)redValue, (uint32_t)irValue);
      if (strlen(ppgPayload) + strlen(tempStr) < sizeof(ppgPayload)) {
        strcat(ppgPayload, tempStr);
        ppgSampleCount++;
      }

      // store for SpO2
      redBuffer[bufferIndex] = redValue;
      irBuffer[bufferIndex] = irValue;
      bufferIndex++;
      if (bufferIndex >= BUFFER_SIZE_LOCAL) { bufferIndex = 0; bufferFull = true; }

      // BPM
      if (checkForBeat(irValue) == true) {
        long delta = millis() - lastBeatTime;
        lastBeatTime = millis();
        float beatsPerMinute = 60000.0 / delta;
        if (beatsPerMinute > 50 && beatsPerMinute < 150) {
          if (beatAvg == 0) beatAvg = beatsPerMinute;
          else beatAvg = (beatAvg * 0.2) + (beatsPerMinute * 0.8);
          finalBPM = filter3((uint8_t)beatAvg);
        }
      }

      // SpO2 calculation
      if (bufferFull) {
        uint32_t minRed = redBuffer[0], maxRed = redBuffer[0];
        uint32_t minIR = irBuffer[0], maxIR = irBuffer[0];
        for (int i = 1; i < BUFFER_SIZE_LOCAL; i++) {
          if (redBuffer[i] < minRed) minRed = redBuffer[i];
          if (redBuffer[i] > maxRed) maxRed = redBuffer[i];
          if (irBuffer[i] < minIR) minIR = irBuffer[i];
          if (irBuffer[i] > maxIR) maxIR = irBuffer[i];
        }
        long acRed = maxRed - minRed;
        long dcRed = minRed;
        long acIR = maxIR - minIR;
        long dcIR = minIR;
        if (acIR > 0 && dcRed > 0) {
          float rValue = ((float)acRed / dcRed) / ((float)acIR / dcIR);
          float spo2Calculated = 110.0 - (17.0 * rValue);
          if (spo2Calculated > 100.0) spo2Calculated = 100.0;
          if (spo2Calculated >= 80.0 && spo2Calculated <= 100.0) {
            finalSpO2 = filter3((uint8_t)spo2Calculated);
          }
        }
      }
      
      // LOG DATA RA SERIAL CHO PYTHON ĐỌC (CSV FORMAT)
      Serial.printf("%lu,%lu,%lu,%u,%u,%d\n", millis(), (uint32_t)irValue, (uint32_t)redValue, finalBPM, finalSpO2, AccelManager_isMoving() ? 1 : 0);
      
    }
    particleSensor.nextSample();
  }
}

bool PPGManager_popNoFingerEvent() {
  if (noFingerEventFlag) {
    noFingerEventFlag = false;
    return true;
  }
  return false;
}

long PPGManager_readIR() {
  // Use library helper to read current IR FIFO or direct IR register
  // MAX30105 provides getIR() which reads latest sample
  return particleSensor.getIR();
}

bool PPGManager_popPacket(char* outBuf, size_t bufSize, size_t* outLen) {
  if (ppgSampleCount >= PPG_PACKET_SIZE_LOCAL) {
    size_t len = strlen(ppgPayload);
    if (len >= bufSize) return false;
    memcpy(outBuf, ppgPayload, len+1);
    if (outLen) *outLen = len;
    ppgPayload[0] = '\0';
    ppgSampleCount = 0;
    return true;
  }
  return false;
}

uint8_t PPGManager_getBPM() { return finalBPM; }
uint8_t PPGManager_getSpO2() { return finalSpO2; }
