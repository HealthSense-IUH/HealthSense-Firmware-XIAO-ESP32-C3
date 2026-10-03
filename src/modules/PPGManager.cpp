#include "PPGManager.h"
#include <MAX30105.h>
#include <heartRate.h>
#include <Arduino.h>
#include "AccelManager.h"
#include "BleProtocol.h"
#include "../config.h"

static MAX30105 particleSensor;
static volatile bool dataReady = false;

// Gói BLE: gom PPG_PACKET_SAMPLES mẫu "millis,red,ir,bpm,spo2" rồi mới gửi
#define PPG_PACKET_SAMPLES 10
static char ppgPayload[512] = "";
static size_t ppgPayloadLen = 0;
static uint8_t ppgSampleCount = 0;

// Cửa sổ tính SpO2 (~1,7 giây ở 100Hz)
#define SPO2_WINDOW 167
#define SPO2_RECALC_EVERY 100  // 100Hz -> tính lại mỗi giây
static uint32_t redBuffer[SPO2_WINDOW];
static uint32_t irBuffer[SPO2_WINDOW];
static int bufferIndex = 0;
static bool bufferFull = false;

static uint8_t finalBPM = 0;
static uint8_t finalSpO2 = 0;
static long lastBeatTime = 0;
static float beatAvg = 0;

// Phát hiện tháo thiết bị: IR dưới ngưỡng liên tục NO_FINGER_MS
static unsigned long noFingerStartTime = 0;
static bool noFingerEventFlag = false;

// Lọc trung vị 3 mẫu gần nhất để bỏ giá trị nhảy vọt
struct Median3 {
  uint8_t v[3] = {0, 0, 0};
  uint8_t push(uint8_t value) {
    v[0] = v[1]; v[1] = v[2]; v[2] = value;
    uint8_t x = v[0], y = v[1], z = v[2];
    if ((x - y) * (z - x) >= 0) return x;
    if ((y - x) * (z - y) >= 0) return y;
    return z;
  }
};
static Median3 bpmFilter;
static Median3 spo2Filter;

void IRAM_ATTR PPGManager_handleInterrupt() {
  dataReady = true;
}

/** Xóa kết quả đo và cửa sổ SpO2 (khi đổi cấu hình cảm biến hoặc tháo thiết bị). */
static void resetMeasurement() {
  bufferIndex = 0;
  bufferFull = false;
  finalBPM = 0;
  finalSpO2 = 0;
  beatAvg = 0;
}

/** Cấu hình đo đầy đủ: 400Hz, trung bình 4 mẫu, LED đỏ + IR (dùng khi bật máy, MEASURE và pha 1 sàng lọc). */
static void configureFullPower() {
  particleSensor.setup(30, 4, 2, 400, 215, 16384);
  particleSensor.setPulseAmplitudeRed(85);
  particleSensor.setPulseAmplitudeIR(85);
}

bool PPGManager_begin(uint8_t interruptPin) {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    return false;
  }

  Wire.setTimeOut(25);
  configureFullPower();

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

/** Thêm một mẫu vào gói BLE đang gom (bỏ mẫu nếu gói đã đầy). */
static void appendToPacket(long redValue, long irValue) {
  char sample[64];
  int len = snprintf(sample, sizeof(sample), FMT_PPG_SAMPLE, (unsigned long)millis(),
                     (unsigned long)redValue, (unsigned long)irValue, finalBPM, finalSpO2);
  if (len > 0 && ppgPayloadLen + (size_t)len < sizeof(ppgPayload)) {
    memcpy(ppgPayload + ppgPayloadLen, sample, (size_t)len + 1);
    ppgPayloadLen += (size_t)len;
    ppgSampleCount++;
  }
}

/** BPM từ khoảng cách giữa 2 nhịp, làm mượt rồi lọc trung vị. */
static void updateHeartRate(long irValue) {
  if (!checkForBeat(irValue)) return;
  long delta = millis() - lastBeatTime;
  lastBeatTime = millis();
  float beatsPerMinute = 60000.0 / delta;
  if (beatsPerMinute > 40 && beatsPerMinute < 255) {
    beatAvg = beatAvg == 0 ? beatsPerMinute : (beatAvg * 0.2) + (beatsPerMinute * 0.8);
    finalBPM = bpmFilter.push((uint8_t)beatAvg);
  }
}

/** SpO2 theo tỷ số R = (AC/DC đỏ) / (AC/DC hồng ngoại) trên cửa sổ gần nhất, tính lại mỗi giây. */
static void updateSpO2() {
  static uint8_t recalcCounter = 0;
  if (!bufferFull || ++recalcCounter < SPO2_RECALC_EVERY) return;
  recalcCounter = 0;

  uint32_t minRed = redBuffer[0], maxRed = redBuffer[0];
  uint32_t minIR = irBuffer[0], maxIR = irBuffer[0];
  for (int i = 1; i < SPO2_WINDOW; i++) {
    if (redBuffer[i] < minRed) minRed = redBuffer[i];
    if (redBuffer[i] > maxRed) maxRed = redBuffer[i];
    if (irBuffer[i] < minIR) minIR = irBuffer[i];
    if (irBuffer[i] > maxIR) maxIR = irBuffer[i];
  }
  long acRed = maxRed - minRed;
  long dcRed = minRed;
  long acIR = maxIR - minIR;
  long dcIR = minIR;
  if (acIR <= 0 || dcRed <= 0) return;

  float rValue = ((float)acRed / dcRed) / ((float)acIR / dcIR);
  float spo2 = 110.0 - (17.0 * rValue);
  if (spo2 > 100.0) spo2 = 100.0;
  if (spo2 >= 80.0) finalSpO2 = spo2Filter.push((uint8_t)spo2);
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

    if (irValue < WEAR_IR_THRESHOLD) {
      if (noFingerStartTime == 0) noFingerStartTime = millis();
      if (millis() - noFingerStartTime > NO_FINGER_MS) {
        // Đã tháo: xóa kết quả, báo sự kiện; tắt cảm biến là việc của DeviceStateManager
        resetMeasurement();
        noFingerEventFlag = true;
        noFingerStartTime = 0;
        break;
      }
    } else {
      noFingerStartTime = 0;
      appendToPacket(redValue, irValue);

      redBuffer[bufferIndex] = redValue;
      irBuffer[bufferIndex] = irValue;
      if (++bufferIndex >= SPO2_WINDOW) { bufferIndex = 0; bufferFull = true; }

      updateHeartRate(irValue);
      updateSpO2();

#if HS_LOG_PPG_CSV
      // CSV cho scripts/log_to_csv.py: millis,ir,red,bpm,spo2,motion
      Serial.printf("%lu,%lu,%lu,%u,%u,%d\n", (unsigned long)millis(), (unsigned long)irValue,
                    (unsigned long)redValue, finalBPM, finalSpO2, AccelManager_isMoving() ? 1 : 0);
#endif
    }
    particleSensor.nextSample();
  }
}

bool PPGManager_popNoFingerEvent() {
  if (!noFingerEventFlag) return false;
  noFingerEventFlag = false;
  return true;
}

long PPGManager_readIR() {
  return particleSensor.getIR();
}

bool PPGManager_popPacket(char* outBuf, size_t bufSize, size_t* outLen) {
  if (ppgSampleCount < PPG_PACKET_SAMPLES || ppgPayloadLen >= bufSize) return false;
  memcpy(outBuf, ppgPayload, ppgPayloadLen + 1);
  if (outLen) *outLen = ppgPayloadLen;
  PPGManager_discardPacket();
  return true;
}

void PPGManager_discardPacket() {
  ppgPayload[0] = '\0';
  ppgPayloadLen = 0;
  ppgSampleCount = 0;
}

uint8_t PPGManager_getBPM() { return finalBPM; }
uint8_t PPGManager_getSpO2() { return finalSpO2; }

void PPGManager_setupPhase1() {
  configureFullPower();
  particleSensor.enableDATARDY();
  resetMeasurement();
}

void PPGManager_setupPhase2(bool lowPower) {
  // 100Hz, trung bình 2 mẫu; tiết kiệm pin: LED đỏ gần tắt (đã có SpO2 từ pha 1), chỉ cần IR cho nhịp tim
  particleSensor.setup(30, 2, 2, 100, 215, 16384);
  particleSensor.setPulseAmplitudeRed(lowPower ? 5 : 75);
  particleSensor.setPulseAmplitudeIR(75);
  particleSensor.enableDATARDY();
  resetMeasurement();
}
