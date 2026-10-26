#include "DeviceStateManager.h"
#include "AfibScreening.h"
#include "comm/BLEManager.h"
#include "config/BleProtocol.h"
#include "config/config.h"
#include "drivers/AccelManager.h"
#include "drivers/DisplayPower.h"
#include "drivers/PPGManager.h"

// ====================
// Cấu hình trạng thái khẩn cấp (Emergency / SOS) - Đồng bộ từ XIAO-nRF52840
// ====================
static constexpr unsigned long EMERGENCY_CANCEL_WINDOW_MS = 15000UL;
static constexpr unsigned long EMERGENCY_IMMOBILE_WINDOW_MS = 10000UL;
static constexpr unsigned long EMERGENCY_FALL_CONFIRM_WINDOW_MS = 2000UL;
static constexpr unsigned long EMERGENCY_PROBE_INTERVAL_MS = 500UL;
static constexpr long EMERGENCY_NO_FINGER_IR_THRESHOLD = 50000L;
static constexpr uint8_t EMERGENCY_BPM_LOW_THRESHOLD = 40;
static constexpr uint8_t EMERGENCY_BPM_HIGH_THRESHOLD = 150;
static constexpr uint8_t EMERGENCY_SPO2_THRESHOLD = 90;
static constexpr unsigned long EMERGENCY_IR_SETTLE_MS = 1500UL;      // Chờ cảm biến ổn định trước khi xét không đeo
static constexpr uint8_t EMERGENCY_NO_FINGER_CONFIRM_COUNT = 3;      // 3 lần thăm dò IR thấp liên tiếp (~1.5s)
static constexpr unsigned long EMERGENCY_ALERT_INTERVAL_MS = 2000UL; // Tần suất tối đa của SOS:ALERT

enum EmergencyType {
  EMERGENCY_NONE,
  EMERGENCY_FALL,
  EMERGENCY_SYNCOPE,
  EMERGENCY_CARDIAC
};

// ====================
// Trạng thái chế độ, nút bấm và thời gian hồi phục
// ====================
static DeviceMode currentMode = MODE_IDLE;
static DeviceMode pendingMode = MODE_IDLE;
static uint8_t wakeButtonPin;
static uint8_t accelVddPin;

// Nút bấm và LED
static unsigned long btnPressTime = 0;
static bool isBtnPressed = false;
static bool isLongPressHandled = false;
static unsigned long ledWakeTime = 0;
static bool isLEDOn = false;

// Dò đeo tay khi IDLE
static unsigned long lastWearCheck = 0;

// Hồi phục tim sau tập: trong 10 phút sau WORKOUT_STOP không tự đo / nhận lệnh sàng lọc AFib
static unsigned long lastWorkoutEndTime = 0;

// Trạng thái khẩn cấp (Emergency)
static bool emergencyActive = false;
static bool emergencyCountdownActive = false;
static EmergencyType emergencyType = EMERGENCY_NONE;
static unsigned long emergencyStartedAt = 0;
static unsigned long emergencyAlarmStartedAt = 0;
static unsigned long emergencyLastProbeAt = 0;
static uint8_t emergencyCancelSecondsLeft = 0;
static uint8_t emergencyBpm = 0;
static uint8_t emergencySpo2 = 0;
static long emergencyIr = 0;
static bool emergencySimulated = false;           // true khi kích hoạt bằng CMD:TEST_SOS
static uint8_t emergencyNoFingerCount = 0;
static unsigned long emergencyLastAlertAt = 0;

static void enterMode(DeviceMode mode);
static void exitMode(DeviceMode mode);
static void requestMode(DeviceMode newMode);
static void DeviceStateManager_enterEmergency(EmergencyType type);
static void DeviceStateManager_processEmergency();
static void DeviceStateManager_abortEmergency(const char* reason);
static void DeviceStateManager_emitEmergencyStatus(const char* payload);

static const char* DeviceStateManager_emergencyTypeName(EmergencyType type) {
  return type == EMERGENCY_FALL ? "FALL" :
         type == EMERGENCY_SYNCOPE ? "SYNCOPE" : "CARDIAC";
}

static void DeviceStateManager_emitEmergencyStatus(const char* payload) {
  Serial.printf("[EMERGENCY] TX connected=%d: %s", BLEManager_isConnected() ? 1 : 0, payload);
  BLEManager_sendEvent(payload);
  BLEManager_notifyReport(payload, strlen(payload));
}

// ====================
// Lệnh từ điện thoại
// ====================
static void onBLECommand(const char* cmd) {
  if (strcmp(cmd, CMD_START_MEASURE) == 0) {
    DeviceStateManager_onEvent(EVT_BLE_START_MEASURE);
  } else if (strcmp(cmd, CMD_START_WORKOUT) == 0 || strcmp(cmd, CMD_WORKOUT_START) == 0) {
    lastWorkoutEndTime = 0;  // Bài tập mới: bỏ thời gian hồi phục của bài trước
    DeviceStateManager_onEvent(EVT_BLE_START_WORKOUT);
  } else if (strcmp(cmd, CMD_WORKOUT_PAUSE) == 0) {
    Serial.println("[WORKOUT] Tạm dừng đo nhịp tim & đếm bước");
    BLEManager_sendEvent(ACK_WORKOUT_PAUSE);
  } else if (strcmp(cmd, CMD_WORKOUT_RESUME) == 0) {
    Serial.println("[WORKOUT] Tiếp tục đo nhịp tim");
    BLEManager_sendEvent(ACK_WORKOUT_RESUME);
  } else if (strcmp(cmd, CMD_WORKOUT_STOP) == 0) {
    Serial.println("[WORKOUT] Kết thúc phiên tập luyện, bắt đầu 10 phút hồi phục tim tĩnh");
    lastWorkoutEndTime = millis();
    BLEManager_sendEvent(ACK_WORKOUT_STOP);
    requestMode(MODE_IDLE);
  } else if (strcmp(cmd, CMD_START_SCREENING) == 0) {
    DeviceStateManager_onEvent(EVT_BLE_START_SCREENING);
  } else if (strcmp(cmd, CMD_IDLE) == 0) {
    DeviceStateManager_onEvent(EVT_NOT_WEARING);  // Về IDLE
  } else if (strcmp(cmd, CMD_CANCEL_SOS) == 0) {
    DeviceStateManager_onEvent(EVT_BLE_CANCEL_SOS);
  } else if (strcmp(cmd, CMD_TEST_SOS) == 0) {
    DeviceStateManager_onEvent(EVT_BLE_TEST_SOS);
  } else {
    Serial.print("[BLE-RX] Lệnh không xác định: ");
    Serial.println(cmd);
  }
}

void DeviceStateManager_handleBLECommand(const char* cmd) {
  onBLECommand(cmd);
}

// ====================
// Hồi phục sau tập và dò đeo tay khi IDLE
// ====================
static bool inWorkoutCooldown() {
  return lastWorkoutEndTime > 0 && (millis() - lastWorkoutEndTime < WORKOUT_COOLDOWN_MS);
}

/** IDLE: mỗi 3 giây bật cảm biến chốc lát; thấy đang đeo thì tự vào sàng lọc (trừ lúc hồi phục sau tập). */
static void idleLoop() {
  if (inWorkoutCooldown()) {
    PPGManager_shutDown();
    return;
  }
  if (millis() - lastWearCheck < WEAR_CHECK_INTERVAL_MS) return;
  lastWearCheck = millis();

  PPGManager_wakeUp();
  delay(50);
  if (PPGManager_readIR() > WEAR_IR_THRESHOLD) {
    Serial.println("[WEAR] Kich hoat do nhip tim!");
    requestMode(MODE_SCREENING);
  } else {
    PPGManager_shutDown();
  }
}

// ====================
// API
// ====================

void DeviceStateManager_begin(uint8_t buttonPin, uint8_t accelVdd) {
  currentMode = MODE_TURN_ON;
  pendingMode = MODE_TURN_ON;
  wakeButtonPin = buttonPin;
  accelVddPin = accelVdd;
  emergencyActive = false;
  emergencyCountdownActive = false;
  emergencyType = EMERGENCY_NONE;
  emergencyStartedAt = 0;
  emergencyAlarmStartedAt = 0;
  emergencyLastProbeAt = 0;
  emergencyCancelSecondsLeft = 0;
  emergencyBpm = 0;
  emergencySpo2 = 0;
  emergencyIr = 0;
  emergencySimulated = false;
  emergencyNoFingerCount = 0;
  emergencyLastAlertAt = 0;
  BLEManager_setCommandCallback(onBLECommand);
  enterMode(MODE_TURN_ON);
}

void DeviceStateManager_handleButton() {
  if (digitalRead(wakeButtonPin) == LOW) {
    if (!isBtnPressed) {
      isBtnPressed = true;
      btnPressTime = millis();
      isLongPressHandled = false;
    } else if (!isLongPressHandled && (millis() - btnPressTime >= LONG_PRESS_MS)) {
      isLongPressHandled = true;
      DeviceStateManager_onEvent(EVT_BUTTON_LONG);
    }
  } else if (isBtnPressed) {
    isBtnPressed = false;
    if (!isLongPressHandled) DeviceStateManager_onEvent(EVT_BUTTON_SHORT);
  }

  if (isLEDOn && (millis() - ledWakeTime > LED_TIMEOUT_MS)) {
    DisplayPower_showOff();
    isLEDOn = false;
  }
}

// ====================
// Xử lý sự kiện thiết bị
// ====================
static void flashLED() {
  DisplayPower_showOn();
  isLEDOn = true;
  ledWakeTime = millis();
}

void DeviceStateManager_onEvent(DeviceEvent event) {
  switch (event) {
    case EVT_BUTTON_SHORT:
      if (currentMode == MODE_EMERGENCY && emergencyActive) {
        Serial.println("event BUTTON_SHORT -> hủy SOS tại thiết bị");
        DeviceStateManager_abortEmergency("BUTTON");
        break;
      }
      flashLED();
      Serial.println("event BUTTON_SHORT, LED on");
      break;

    case EVT_BUTTON_LONG:
      Serial.println("event BUTTON_LONG, device shutdown");
      requestMode(MODE_SHUTDOWN);
      break;

    case EVT_MOTION:
      if (currentMode != MODE_MEASURE && currentMode != MODE_SCREENING) break;
      Serial.printf("[%lu] event MOTION, detected arm motion\n", (unsigned long)millis());
      BLEManager_sendEvent(EVT_MSG_MOTION);
      if (currentMode == MODE_MEASURE) {
        // Đo chủ động bị rung tay: về IDLE chờ người dùng đo lại
        requestMode(MODE_IDLE);
      } else {
        AfibScreening_onMotion();
      }
      break;

    case EVT_NOT_WEARING:
      Serial.println("event NOT_WEARING");
      if (currentMode == MODE_EMERGENCY && emergencyActive) {
        if (!emergencySimulated) {
          Serial.println("[EMERGENCY] Phát hiện không đeo tay -> hủy SOS");
          DeviceStateManager_abortEmergency("NO_WEAR");
        }
        break;
      }
      BLEManager_sendEvent(EVT_MSG_NOT_WEARING);
      requestMode(MODE_IDLE);
      break;

    case EVT_FALL_DETECTED:
      if (currentMode == MODE_EMERGENCY || pendingMode == MODE_EMERGENCY) {
        Serial.println("event FALL_DETECTED (bỏ qua - đang trong EMERGENCY)");
        break;
      }
      Serial.println("event FALL_DETECTED");
      DeviceStateManager_enterEmergency(EMERGENCY_FALL);
      break;

    case EVT_BLE_CANCEL_SOS:
      Serial.println("event BLE_CANCEL_SOS");
      if (currentMode == MODE_EMERGENCY && emergencyActive) {
        DeviceStateManager_abortEmergency(nullptr);
      }
      break;

    case EVT_BLE_TEST_SOS:
      Serial.println("event BLE_TEST_SOS");
      emergencySimulated = true;
      DeviceStateManager_enterEmergency(EMERGENCY_FALL);
      break;

    case EVT_BLE_START_MEASURE:
      Serial.println("event BLE_START_MEASURE");
      requestMode(MODE_MEASURE);
      break;

    case EVT_BLE_START_WORKOUT:
      Serial.println("event BLE_START_WORKOUT");
      requestMode(MODE_WORKOUT);
      break;

    case EVT_BLE_START_SCREENING:
      Serial.println("event BLE_START_SCREENING");
      if (currentMode == MODE_WORKOUT) {
        Serial.println("[WARN] Từ chối đo AFib Screening do đang trong MODE_WORKOUT");
        BLEManager_sendEvent(ERR_WORKOUT_IN_PROGRESS);
        break;
      }
      if (inWorkoutCooldown()) {
        Serial.println("[WARN] Từ chối đo AFib Screening do đang trong 10 phút hồi phục tim sau tập");
        BLEManager_sendEvent(ERR_WORKOUT_COOLDOWN);
        break;
      }
      BLEManager_sendEvent(EVT_MSG_START_SCREENING);
      if (currentMode == MODE_SCREENING) {
        // Đang sàng lọc (kể cả lúc chờ giữa các pha): bắt đầu lại chu kỳ ngay
        exitMode(MODE_SCREENING);
        enterMode(MODE_SCREENING);
      } else {
        requestMode(MODE_SCREENING);
      }
      break;
  }
}

// ====================
// Vòng lặp và chuyển đổi chế độ
// ====================
void DeviceStateManager_loop() {
  if (pendingMode != currentMode) {
    exitMode(currentMode);
    currentMode = pendingMode;
    enterMode(currentMode);
  }

  if (currentMode == MODE_IDLE) idleLoop();
  else if (currentMode == MODE_SCREENING) AfibScreening_loop();
  else if (currentMode == MODE_EMERGENCY) DeviceStateManager_processEmergency();
}

DeviceMode DeviceStateManager_getMode() {
  return currentMode;
}

// Đổi chế độ không làm ngay mà để DeviceStateManager_loop() thực hiện (exitMode -> enterMode)
static void requestMode(DeviceMode newMode) {
  if (newMode == currentMode || newMode == pendingMode) return;
  pendingMode = newMode;
}

static void enterMode(DeviceMode mode) {
  switch (mode) {
    case MODE_IDLE:
      Serial.println("Mode IDLE");
      PPGManager_shutDown();
      AccelManager_setMotionThreshold(MOTION_THRESHOLD_IDLE);
      // Luôn quảng bá BLE khi IDLE để điện thoại tìm thấy và kết nối được
      if (!BLEManager_isConnected()) {
        BLEManager_startAdvertising();
        Serial.println("[BLE] Đang bật advertising (IDLE)");
      }
      break;

    case MODE_MEASURE:
      Serial.println("Mode MEASURE");
      PPGManager_wakeUp();
      AccelManager_setMotionThreshold(MOTION_THRESHOLD_MEASURE);
      break;

    case MODE_WORKOUT:
      Serial.println("Mode WORKOUT");
      PPGManager_wakeUp();
      AccelManager_setMotionThreshold(MOTION_THRESHOLD_WORKOUT);
      break;

    case MODE_SCREENING:
      Serial.println("Mode SCREENING");
      AfibScreening_start();
      AccelManager_setMotionThreshold(MOTION_THRESHOLD_SCREENING);
      break;

    case MODE_EMERGENCY:
      Serial.println("Mode EMERGENCY");
      emergencyActive = true;
      emergencyCountdownActive = true;
      emergencyType = emergencyType == EMERGENCY_NONE ? EMERGENCY_FALL : emergencyType;
      emergencyAlarmStartedAt = millis();
      emergencyStartedAt = millis();
      emergencyLastProbeAt = millis();
      emergencyLastAlertAt = 0;
      emergencyNoFingerCount = 0;
      emergencyCancelSecondsLeft = 15;
      emergencyBpm = 0;
      emergencySpo2 = 0;
      emergencyIr = 0;

      PPGManager_wakeUp();
      PPGManager_setupPhase1();
      DisplayPower_showRed();
      isLEDOn = true;
      ledWakeTime = millis();
      Serial.printf("[EMERGENCY] Enter type=%s simulated=%d connected=%d\n",
                    DeviceStateManager_emergencyTypeName(emergencyType), emergencySimulated ? 1 : 0,
                    BLEManager_isConnected() ? 1 : 0);
      DeviceStateManager_emitEmergencyStatus("SOS:COUNTDOWN,15\n");
      break;

    case MODE_SHUTDOWN:
      Serial.println("Mode SHUTDOWN");
      PPGManager_shutDown();
      DisplayPower_showRed();  // Báo đang tắt: đỏ 1 giây
      delay(1000);
      DisplayPower_showOff();
      digitalWrite(accelVddPin, LOW);  // Ngắt nguồn MPU6050 khi deep sleep
      Serial.println("[POWER] Đã tắt nguồn MPU6050");
      // Thả nổi I2C để không rò dòng qua điện trở kéo lên khi cảm biến đã mất nguồn
      pinMode(I2C_SDA_PIN, INPUT);
      pinMode(I2C_SCL_PIN, INPUT);
      delay(100);
      esp_deep_sleep_enable_gpio_wakeup(1ULL << wakeButtonPin, ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
      break;

    case MODE_TURN_ON:
      Serial.println("Mode TURN_ON");
      flashLED();
      requestMode(MODE_IDLE);
      break;
  }
}

static void exitMode(DeviceMode mode) {
  switch (mode) {
    case MODE_SCREENING:
      AfibScreening_stop();
      break;

    case MODE_IDLE:
      BLEManager_startAdvertising();
      Serial.println("[BLE] Đã bật advertising (thoát IDLE)");
      break;

    case MODE_EMERGENCY:
      DisplayPower_showOff();
      emergencyActive = false;
      emergencyCountdownActive = false;
      emergencySimulated = false;
      emergencyNoFingerCount = 0;
      emergencyType = EMERGENCY_NONE;
      break;

    default:
      break;
  }
}

static void DeviceStateManager_enterEmergency(EmergencyType type) {
  emergencyType = type;
  Serial.println("[EMERGENCY] Enter request from fall/syncope trigger");
  if (currentMode != MODE_EMERGENCY) {
    requestMode(MODE_EMERGENCY);
  }
}

static void DeviceStateManager_abortEmergency(const char* reason) {
  char payload[48];
  if (reason != nullptr && reason[0] != '\0') {
    snprintf(payload, sizeof(payload), "SOS:CANCELLED,%s\n", reason);
  } else {
    snprintf(payload, sizeof(payload), "SOS:CANCELLED\n");
  }
  DeviceStateManager_emitEmergencyStatus(payload);
  DisplayPower_showOff();
  PPGManager_shutDown();
  emergencyActive = false;
  emergencyCountdownActive = false;
  emergencySimulated = false;
  emergencyNoFingerCount = 0;
  emergencyType = EMERGENCY_NONE;
  requestMode(MODE_IDLE);
}

static void DeviceStateManager_processEmergency() {
  if (!emergencyActive) return;

  unsigned long now = millis();

  // 1. Quản lý đếm ngược (15s hủy)
  if (emergencyCountdownActive) {
    if (now - emergencyAlarmStartedAt >= EMERGENCY_CANCEL_WINDOW_MS) {
      emergencyCountdownActive = false;
      char confirmed[40];
      snprintf(confirmed, sizeof(confirmed), "SOS:CONFIRMED,%s\n", DeviceStateManager_emergencyTypeName(emergencyType));
      DeviceStateManager_emitEmergencyStatus(confirmed);
    } else {
      uint8_t secondsLeft = (uint8_t)((EMERGENCY_CANCEL_WINDOW_MS - (now - emergencyAlarmStartedAt) + 999UL) / 1000UL);
      if (secondsLeft != emergencyCancelSecondsLeft) {
        emergencyCancelSecondsLeft = secondsLeft;
        char countdown[32];
        snprintf(countdown, sizeof(countdown), "SOS:COUNTDOWN,%u\n", emergencyCancelSecondsLeft);
        DeviceStateManager_emitEmergencyStatus(countdown);
        // Nháy LED đỏ báo động
        if (secondsLeft % 2 == 0) DisplayPower_showRed();
        else DisplayPower_showOff();
      }
    }
  }

  // 2. Thăm dò nhịp tim & kiểm tra đeo tay
  if (now - emergencyLastProbeAt >= EMERGENCY_PROBE_INTERVAL_MS) {
    emergencyLastProbeAt = now;
    emergencyIr = PPGManager_readIR();
    emergencyBpm = PPGManager_getBPM();
    emergencySpo2 = PPGManager_getSpO2();
    Serial.printf("[EMERGENCY] probe t=%lums IR=%ld BPM=%u SpO2=%u lowIR=%u\n",
                  (unsigned long)(now - emergencyStartedAt), emergencyIr, emergencyBpm, emergencySpo2,
                  emergencyNoFingerCount);

    if (!emergencySimulated && (now - emergencyStartedAt) >= EMERGENCY_IR_SETTLE_MS) {
      if (emergencyIr < EMERGENCY_NO_FINGER_IR_THRESHOLD) {
        emergencyNoFingerCount++;
        if (emergencyNoFingerCount >= EMERGENCY_NO_FINGER_CONFIRM_COUNT) {
          Serial.println("[EMERGENCY] IR thấp liên tiếp -> không đeo tay, hủy SOS");
          DeviceStateManager_abortEmergency("NO_WEAR");
          return;
        }
      } else {
        emergencyNoFingerCount = 0;
      }
    }

    bool abnormalVitals =
        (emergencyBpm > 0 && emergencyBpm < EMERGENCY_BPM_LOW_THRESHOLD) ||
        (emergencyBpm > EMERGENCY_BPM_HIGH_THRESHOLD) ||
        (emergencySpo2 > 0 && emergencySpo2 < EMERGENCY_SPO2_THRESHOLD);

    bool immobile = !AccelManager_isMoving() && (now - emergencyStartedAt >= EMERGENCY_IMMOBILE_WINDOW_MS);
    if ((abnormalVitals || immobile) && (now - emergencyLastAlertAt >= EMERGENCY_ALERT_INTERVAL_MS)) {
      emergencyLastAlertAt = now;
      if (abnormalVitals) {
        char payload[96];
        snprintf(payload, sizeof(payload), "SOS:ALERT,%s,ACTIVE,%lu,%u,%u,%ld\n",
                 DeviceStateManager_emergencyTypeName(emergencyType), (unsigned long)now,
                 emergencyBpm, emergencySpo2, emergencyIr);
        DeviceStateManager_emitEmergencyStatus(payload);
      } else {
        DeviceStateManager_emitEmergencyStatus("SOS:ALERT,SYNCOPE,ACTIVE\n");
      }
    }

    if (emergencyType == EMERGENCY_FALL && (now - emergencyStartedAt >= EMERGENCY_FALL_CONFIRM_WINDOW_MS)) {
      if (!abnormalVitals && AccelManager_isMoving()) {
        emergencyType = EMERGENCY_SYNCOPE;
      }
    }

    if (emergencyType == EMERGENCY_FALL && emergencyCountdownActive && (now - emergencyStartedAt >= EMERGENCY_CANCEL_WINDOW_MS)) {
      emergencyType = EMERGENCY_CARDIAC;
    }
  }
}
