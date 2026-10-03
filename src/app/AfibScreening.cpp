#include "AfibScreening.h"
#include <Arduino.h>
#include "comm/BLEManager.h"
#include "config/BleProtocol.h"
#include "drivers/PPGManager.h"

/*
 * Sàng lọc AFib: chu kỳ 10 phút, mốc tính từ đầu chu kỳ.
 *   Pha 1 (0s):     đo 60s đầy đủ (BPM + SpO2) -> báo R1
 *   Pha 2 (150s), Pha 3 (300s), Pha 4 (450s): đo 30s (chủ yếu BPM, LED đỏ gần tắt) -> báo R2
 *   600s:           bắt đầu chu kỳ mới
 * Giữa các pha cảm biến tắt để tiết kiệm pin. Rung tay trong pha 1 thì ngủ 30s rồi đo lại (tối đa 3 lần);
 * hỏng cả 3 lần hoặc không còn đủ 60s trước mốc 150s thì bỏ SpO2 của chu kỳ, các pha sau đo cả SpO2.
 * Giá trị enum giữ như bản cũ: pha đo phụ là số lẻ, pha chờ trước nó là số chẵn liền trước.
 */
enum ScreeningPhase : uint8_t {
  PHASE_NONE = 0,
  PHASE1_MEASURE = 1,
  WAIT_PHASE2 = 2,
  PHASE2_MEASURE = 3,
  WAIT_PHASE3 = 4,
  PHASE3_MEASURE = 5,
  WAIT_PHASE4 = 6,
  PHASE4_MEASURE = 7,
  WAIT_CYCLE_END = 8,
  PHASE1_RETRY_SLEEP = 9,
};

#define PHASE1_DURATION_MS      60000UL
#define SUB_PHASE_DURATION_MS   30000UL
#define RETRY_SLEEP_MS          30000UL
#define MAX_PHASE1_ATTEMPTS     3
#define PHASE2_START_MS         150000UL  // 2,5 phút
#define SALVAGE_MIN_SAMPLES     10        // Rung tay giữa pha 1: vẫn báo R1 nếu đã có hơn 10 mẫu
#define REPORT_MIN_BPM          40

static ScreeningPhase screeningPhase = PHASE_NONE;
static unsigned long screeningCycleStart = 0;
static uint8_t screeningRetryCount = 0;
static unsigned long phaseStartTime = 0;
static unsigned long phaseSleepStart = 0;
static unsigned long phaseSleepDuration = 0;
static bool isSpo2Missed = false;
static uint8_t phase1AvgSpO2 = 0;

// Cộng dồn BPM / SpO2 mỗi giây để lấy trung bình của pha
static unsigned long bpmSum = 0;
static unsigned long spo2Sum = 0;
static unsigned long validSampleCount = 0;
static unsigned long lastSampleAccumTime = 0;

static void resetAccumulator() {
  bpmSum = 0;
  spo2Sum = 0;
  validSampleCount = 0;
  lastSampleAccumTime = millis();
}

/** Bắt đầu một pha đo: xóa cộng dồn, bật cảm biến với cấu hình của pha. */
static void startMeasurePhase(ScreeningPhase phase) {
  screeningPhase = phase;
  phaseStartTime = millis();
  resetAccumulator();
  PPGManager_wakeUp();
  if (phase == PHASE1_MEASURE) PPGManager_setupPhase1();
  else PPGManager_setupPhase2(!isSpo2Missed);  // Đã có SpO2 từ pha 1 thì chạy chế độ tiết kiệm pin
}

/** Chờ (cảm biến đã tắt) tới mốc offsetMs tính từ đầu chu kỳ. */
static void sleepUntil(ScreeningPhase waitPhase, unsigned long offsetMs) {
  screeningPhase = waitPhase;
  phaseSleepStart = millis();
  long remaining = (long)(screeningCycleStart + offsetMs) - (long)millis();
  phaseSleepDuration = remaining > 0 ? remaining : 0;
}

static void startCycle() {
  screeningCycleStart = millis();
  screeningRetryCount = 0;
  isSpo2Missed = false;
  startMeasurePhase(PHASE1_MEASURE);
}

static void sendReport(const char* format, uint8_t bpm, uint8_t spo2) {
  if (!BLEManager_isConnected()) return;
  char report[32];
  snprintf(report, sizeof(report), format, bpm, spo2);
  BLEManager_notifyReport(report, strlen(report));
}

/** Cộng dồn mỗi giây. Pha 1 cần cả BPM và SpO2; pha phụ chỉ cần BPM (SpO2 chỉ cộng khi đã lỡ ở pha 1). */
static void accumulateVitals(bool phase1) {
  unsigned long now = millis();
  if (now - lastSampleAccumTime < 1000) return;
  lastSampleAccumTime = now;
  uint8_t bpm = PPGManager_getBPM();
  uint8_t spo2 = PPGManager_getSpO2();
  if (phase1) {
    if (bpm == 0 || spo2 == 0) return;
    bpmSum += bpm;
    spo2Sum += spo2;
  } else {
    if (bpm == 0) return;
    bpmSum += bpm;
    if (isSpo2Missed && spo2 > 0) spo2Sum += spo2;
  }
  validSampleCount++;
}

static uint8_t averageBPM() {
  return validSampleCount > 0 ? bpmSum / validSampleCount : 0;
}

static uint8_t averageSpO2() {
  return validSampleCount > 0 ? spo2Sum / validSampleCount : 0;
}

/** Rung tay giữa pha 1: tắt cảm biến, ngủ 30s rồi đo lại; quá 3 lần thì bỏ SpO2 và chờ pha 2. */
static void onPhase1Motion() {
  Serial.println("[SCREENING] Phát hiện chuyển động! Ngừng đo Pha 1 và thử lại chớp nhoáng...");

  // Cứu dữ liệu: đã có hơn 10 mẫu hợp lệ trước khi rung tay thì vẫn báo R1
  if (validSampleCount > SALVAGE_MIN_SAMPLES && BLEManager_isConnected()) {
    uint8_t avgBPM = averageBPM();
    uint8_t avgSpO2 = averageSpO2();
    sendReport(FMT_REPORT_PHASE1, avgBPM, avgSpO2);
    Serial.printf("[SCREENING] Đã vớt vát dữ liệu sinh hiệu trước khi hủy: %u BPM, %u%%\n", avgBPM, avgSpO2);
  }

  PPGManager_shutDown();
  screeningRetryCount++;

  if (screeningRetryCount < MAX_PHASE1_ATTEMPTS) {
    screeningPhase = PHASE1_RETRY_SLEEP;
    phaseSleepStart = millis();
    Serial.printf("[SCREENING] Micro-Retry lần %u/%u. Ngủ đông 30 giây...\n", screeningRetryCount,
                  MAX_PHASE1_ATTEMPTS);
  } else {
    isSpo2Missed = true;
    Serial.println("[SCREENING] Thất bại 3 lần đo Pha 1. Bỏ lỡ SpO2. Nghỉ đến mốc 2.5 phút (Pha 2)...");
    sleepUntil(WAIT_PHASE2, PHASE2_START_MS);
  }
}

static void finishPhase1() {
  PPGManager_shutDown();
  uint8_t avgBPM = averageBPM();
  uint8_t avgSpO2 = averageSpO2();
  phase1AvgSpO2 = avgSpO2;  // Các pha phụ báo lại SpO2 này
  isSpo2Missed = false;

  Serial.printf("[SCREENING] Pha 1 thành công! Avg BPM: %u | Avg SpO2: %u\n", avgBPM, avgSpO2);
  sendReport(FMT_REPORT_PHASE1, avgBPM, avgSpO2);

  sleepUntil(WAIT_PHASE2, PHASE2_START_MS);
  Serial.printf("[SCREENING] Ngủ động %lu giây để đến mốc 2.5 phút (Pha 2)...\n", phaseSleepDuration / 1000);
}

static void finishSubPhase() {
  PPGManager_shutDown();
  uint8_t avgBPM = averageBPM();
  uint8_t avgSpO2 = isSpo2Missed ? averageSpO2() : 0;
  uint8_t reportBPM = (avgBPM > REPORT_MIN_BPM && avgBPM < 255) ? avgBPM : 0;  // Ngoài khoảng là đo hỏng, báo 0
  uint8_t reportSpO2 = isSpo2Missed ? avgSpO2 : phase1AvgSpO2;
  uint8_t phaseNumber = (screeningPhase / 2) + 1;

  Serial.printf("[SCREENING] Pha %u hoàn tất! Avg BPM: %u (Gửi: %u) | SpO2: %u\n", phaseNumber, avgBPM, reportBPM,
                reportSpO2);
  sendReport(FMT_REPORT_PHASE2, reportBPM, reportSpO2);

  // Pha đo m -> chờ m+1 tới mốc kế tiếp (pha 2 -> 300s, pha 3 -> 450s, pha 4 -> hết chu kỳ 600s)
  unsigned long targetMs = PHASE2_START_MS * phaseNumber;  // Mốc pha kế tiếp = 150s x số thứ tự pha vừa xong
  ScreeningPhase waitPhase = static_cast<ScreeningPhase>(screeningPhase + 1);
  sleepUntil(waitPhase, targetMs);
  Serial.printf("[SCREENING] Ngủ động %lu giây để đến mốc %lus...\n", phaseSleepDuration / 1000, targetMs / 1000);
}

/** Hết 30s ngủ thử lại: còn đủ 60s trước mốc pha 2 thì đo lại pha 1, không thì bỏ SpO2 và chờ pha 2. */
static void finishRetrySleep() {
  long untilPhase2 = (long)(screeningCycleStart + PHASE2_START_MS) - (long)millis();
  if (untilPhase2 < (long)PHASE1_DURATION_MS) {
    Serial.println("[SCREENING] Thời gian còn lại < 60s, không đủ đo trọn vẹn Pha 1. Hủy đo Pha 1 và chuyển sang chờ Pha 2...");
    isSpo2Missed = true;
    sleepUntil(WAIT_PHASE2, PHASE2_START_MS);
  } else {
    Serial.println("[SCREENING] Hết 30 giây ngủ micro-retry. Thức dậy đo lại Pha 1...");
    startMeasurePhase(PHASE1_MEASURE);
  }
}

void AfibScreening_loop() {
  unsigned long now = millis();
  switch (screeningPhase) {
    case PHASE1_MEASURE:
      accumulateVitals(true);
      if (now - phaseStartTime >= PHASE1_DURATION_MS) finishPhase1();
      break;

    case PHASE1_RETRY_SLEEP:
      if (now - phaseSleepStart >= RETRY_SLEEP_MS) finishRetrySleep();
      break;

    case WAIT_PHASE2:
    case WAIT_PHASE3:
    case WAIT_PHASE4:
      if (now - phaseSleepStart >= phaseSleepDuration) {
        ScreeningPhase measurePhase = static_cast<ScreeningPhase>(screeningPhase + 1);
        Serial.printf("[SCREENING] Thức dậy bắt đầu Pha đo thứ %u...\n", (measurePhase / 2) + 1);
        startMeasurePhase(measurePhase);
      }
      break;

    case PHASE2_MEASURE:
    case PHASE3_MEASURE:
    case PHASE4_MEASURE:
      accumulateVitals(false);
      if (now - phaseStartTime >= SUB_PHASE_DURATION_MS) finishSubPhase();
      break;

    case WAIT_CYCLE_END:
      if (now - phaseSleepStart >= phaseSleepDuration) {
        Serial.println("[SCREENING] Đã xong 10 phút chu kỳ. Bắt đầu chu kỳ 10 phút mới (Pha 1)...");
        startCycle();
      }
      break;

    case PHASE_NONE:
      break;
  }
}

void AfibScreening_start() {
  phase1AvgSpO2 = 0;
  startCycle();
}

void AfibScreening_stop() {
  screeningPhase = PHASE_NONE;
}

void AfibScreening_onMotion() {
  if (screeningPhase == PHASE1_MEASURE) onPhase1Motion();
}
