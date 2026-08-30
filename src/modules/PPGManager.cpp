#include "PPGManager.h"
#include <MAX30105.h>
#include <heartRate.h>
#include <spo2_algorithm.h>
#include <Arduino.h>
#include "AccelManager.h"

// Đặt = 0 để tắt log CSV từng mẫu qua Serial (tiết kiệm CPU/pin khi không thu data)
#ifndef PPG_SERIAL_LOG
#define PPG_SERIAL_LOG 1
#endif

static MAX30105 particleSensor;
static const byte* interruptPinPtr = nullptr;
static volatile bool dataReady = false;

#define PPG_PACKET_SIZE_LOCAL 10
static char ppgPayload[512] = "";
static uint8_t ppgSampleCount = 0;

// ===== SpO2: thuật toán Maxim trên buffer 25Hz x 4 giây =====
// Cảm biến chạy 100Hz (Phase 1) hoặc 50Hz (Phase 2); decimate về 25Hz
// bằng trung bình decimFactor mẫu liên tiếp rồi đưa vào buffer trượt 100 mẫu.
#define SPO2_BUFFER_SIZE 100   // 25Hz * 4s (theo yêu cầu của maxim algorithm)
#define SPO2_CALC_INTERVAL 25  // tính lại mỗi 25 mẫu decimated = 1 giây
static uint32_t irBuf25[SPO2_BUFFER_SIZE];
static uint32_t redBuf25[SPO2_BUFFER_SIZE];
static int buf25Head = 0;      // vị trí ghi kế tiếp (ring buffer)
static int buf25Count = 0;     // số mẫu hiện có (tối đa SPO2_BUFFER_SIZE)
static int newDecimatedCount = 0;

static uint8_t decimFactor = 4;  // 100Hz -> 25Hz; Phase 2 (50Hz) dùng 2
static uint32_t decimIrSum = 0;
static uint32_t decimRedSum = 0;
static uint8_t decimCount = 0;

// ===== BPM: median của các khoảng IBI (inter-beat interval) =====
#define IBI_RING_SIZE 8
static uint16_t ibiRing[IBI_RING_SIZE];
static uint8_t ibiCount = 0;
static uint8_t ibiIdx = 0;
static uint8_t ibiRejectStreak = 0;

static uint8_t finalBPM = 0;
static uint8_t finalSpO2 = 0;
static long lastBeatTime = 0;

// no-finger detection (mirror original behavior)
static unsigned long noFingerStartTime = 0;
static volatile bool noFingerEventFlag = false;

void IRAM_ATTR PPGManager_handleInterrupt() {
  dataReady = true;
}

// Median-3, seed bằng giá trị đầu tiên để không bị kéo về 0 lúc khởi động
static uint8_t b_spo2[3] = {0,0,0};
static uint8_t filter3_SpO2(uint8_t newVal) {
  if (b_spo2[0] == 0 && b_spo2[1] == 0 && b_spo2[2] == 0) {
    b_spo2[0] = b_spo2[1] = b_spo2[2] = newVal;
    return newVal;
  }
  b_spo2[0] = b_spo2[1]; b_spo2[1] = b_spo2[2]; b_spo2[2] = newVal;
  uint8_t x=b_spo2[0], y=b_spo2[1], z=b_spo2[2];
  if ((x - y) * (z - x) >= 0) return x;
  if ((y - x) * (z - y) >= 0) return y;
  return z;
}

static uint16_t medianIBI() {
  uint16_t tmp[IBI_RING_SIZE];
  uint8_t n = ibiCount;
  memcpy(tmp, ibiRing, n * sizeof(uint16_t));
  // insertion sort (n <= 8)
  for (uint8_t i = 1; i < n; i++) {
    uint16_t key = tmp[i];
    int8_t j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = key;
  }
  if (n & 1) return tmp[n/2];
  return (tmp[n/2 - 1] + tmp[n/2]) / 2;
}

static void resetAlgoState() {
  buf25Head = 0;
  buf25Count = 0;
  newDecimatedCount = 0;
  decimIrSum = 0;
  decimRedSum = 0;
  decimCount = 0;
  ibiCount = 0;
  ibiIdx = 0;
  ibiRejectStreak = 0;
  finalBPM = 0;
  finalSpO2 = 0;
  lastBeatTime = 0;
  b_spo2[0] = b_spo2[1] = b_spo2[2] = 0;
}

// Nhận 1 mẫu decimated 25Hz, cập nhật buffer trượt và chạy Maxim mỗi 1 giây
static void spo2_pushDecimatedSample(uint32_t irAvg, uint32_t redAvg) {
  irBuf25[buf25Head] = irAvg;
  redBuf25[buf25Head] = redAvg;
  buf25Head = (buf25Head + 1) % SPO2_BUFFER_SIZE;
  if (buf25Count < SPO2_BUFFER_SIZE) buf25Count++;
  newDecimatedCount++;

  if (buf25Count < SPO2_BUFFER_SIZE || newDecimatedCount < SPO2_CALC_INTERVAL) return;
  newDecimatedCount = 0;

  // Linearize ring buffer (mẫu cũ nhất đứng đầu) cho maxim algorithm
  static uint32_t irLin[SPO2_BUFFER_SIZE];
  static uint32_t redLin[SPO2_BUFFER_SIZE];
  int start = buf25Head; // phần tử cũ nhất
  for (int i = 0; i < SPO2_BUFFER_SIZE; i++) {
    int idx = (start + i) % SPO2_BUFFER_SIZE;
    irLin[i] = irBuf25[idx];
    redLin[i] = redBuf25[idx];
  }

  int32_t spo2 = 0, heartRate = 0;
  int8_t spo2Valid = 0, hrValid = 0;
  maxim_heart_rate_and_oxygen_saturation(irLin, SPO2_BUFFER_SIZE, redLin,
                                         &spo2, &spo2Valid, &heartRate, &hrValid);

  if (spo2Valid && spo2 >= 80 && spo2 <= 100) {
    finalSpO2 = filter3_SpO2((uint8_t)spo2);
  }
  // heartRate của Maxim chỉ dùng làm tham khảo; BPM chính lấy từ median IBI
}

// Nhận 1 nhịp tim mới, lọc outlier theo median IBI rồi cập nhật finalBPM
static void bpm_onBeat(long now) {
  long delta = now - lastBeatTime;
  lastBeatTime = now;

  // 240ms..1500ms tương ứng 250..40 BPM
  if (delta < 240 || delta > 1500) return;

  if (ibiCount >= 4) {
    uint16_t med = medianIBI();
    long dev = delta - (long)med;
    if (dev < 0) dev = -dev;
    if (dev > med / 4) {
      // Lệch >25% so với median -> nhiều khả năng nhịp giả, bỏ qua
      ibiRejectStreak++;
      if (ibiRejectStreak >= 5) {
        // Nhịp tim thực sự đã thay đổi nhanh -> làm lại từ đầu
        ibiCount = 0;
        ibiIdx = 0;
        ibiRejectStreak = 0;
      }
      return;
    }
  }
  ibiRejectStreak = 0;

  ibiRing[ibiIdx] = (uint16_t)delta;
  ibiIdx = (ibiIdx + 1) % IBI_RING_SIZE;
  if (ibiCount < IBI_RING_SIZE) ibiCount++;

  if (ibiCount >= 3) {
    uint16_t med = medianIBI();
    if (med > 0) {
      uint16_t bpm = (uint16_t)(60000UL / med);
      if (bpm >= 40 && bpm <= 250) finalBPM = (uint8_t)bpm;
    }
  }
}

bool PPGManager_begin(uint8_t interruptPin) {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    return false;
  }

  Wire.setTimeOut(25);
  particleSensor.setup(30, 4, 2, 400, 215, 16384); // 400Hz / avg 4 = 100Hz
  particleSensor.setPulseAmplitudeRed(85);
  particleSensor.setPulseAmplitudeIR(85);
  decimFactor = 4; // 100Hz -> 25Hz
  resetAlgoState();

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
        resetAlgoState();
        noFingerEventFlag = true;
        noFingerStartTime = 0;
        // leave shutdown to caller
        break; // stop processing FIFO
      }
    } else {
      noFingerStartTime = 0;
      // package for BLE
      char tempStr[64];
      sprintf(tempStr, "%lu,%u,%u,%u,%u\n", millis(), (uint32_t)redValue, (uint32_t)irValue, finalBPM, finalSpO2);
      if (strlen(ppgPayload) + strlen(tempStr) < sizeof(ppgPayload)) {
        strcat(ppgPayload, tempStr);
        ppgSampleCount++;
      }

      // Decimate về 25Hz cho thuật toán SpO2 (Maxim)
      decimIrSum += (uint32_t)irValue;
      decimRedSum += (uint32_t)redValue;
      decimCount++;
      if (decimCount >= decimFactor) {
        spo2_pushDecimatedSample(decimIrSum / decimFactor, decimRedSum / decimFactor);
        decimIrSum = 0;
        decimRedSum = 0;
        decimCount = 0;
      }

      // BPM: phát hiện nhịp trên tín hiệu gốc, lọc outlier bằng median IBI
      if (checkForBeat(irValue) == true) {
        bpm_onBeat(millis());
      }

#if PPG_SERIAL_LOG
      // LOG DATA RA SERIAL CHO PYTHON ĐỌC (CSV FORMAT)
      Serial.printf("%lu,%lu,%lu,%u,%u,%d\n", millis(), (uint32_t)irValue, (uint32_t)redValue, finalBPM, finalSpO2, AccelManager_isMoving() ? 1 : 0);
#endif
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

void PPGManager_setupPhase1() {
  particleSensor.setup(30, 4, 2, 400, 215, 16384); // 400Hz / avg 4 = 100Hz
  particleSensor.setPulseAmplitudeRed(85);
  particleSensor.setPulseAmplitudeIR(85);
  particleSensor.enableDATARDY();
  decimFactor = 4; // 100Hz -> 25Hz
  resetAlgoState();
}

void PPGManager_setupPhase2(bool lowPower) {
  if (lowPower) {
    particleSensor.setup(30, 2, 2, 100, 215, 16384); // 100Hz / avg 2 = 50Hz
    particleSensor.setPulseAmplitudeRed(5);
    particleSensor.setPulseAmplitudeIR(75);
  } else {
    particleSensor.setup(30, 2, 2, 100, 215, 16384);
    particleSensor.setPulseAmplitudeRed(75);
    particleSensor.setPulseAmplitudeIR(75);
  }
  particleSensor.enableDATARDY();
  decimFactor = 2; // 50Hz -> 25Hz
  resetAlgoState();
}
