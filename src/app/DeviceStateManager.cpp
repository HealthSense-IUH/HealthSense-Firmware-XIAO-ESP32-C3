#include "DeviceStateManager.h"
#include "AfibScreening.h"
#include "comm/BLEManager.h"
#include "config/BleProtocol.h"
#include "config/config.h"
#include "drivers/AccelManager.h"
#include "drivers/DisplayPower.h"
#include "drivers/PPGManager.h"

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

static void enterMode(DeviceMode mode);
static void exitMode(DeviceMode mode);
static void requestMode(DeviceMode newMode);

// ---- Lệnh từ điện thoại ----

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
  } else {
    Serial.print("[BLE-RX] Lệnh không xác định: ");
    Serial.println(cmd);
  }
}

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

// ---- API ----

void DeviceStateManager_begin(uint8_t buttonPin, uint8_t accelVdd) {
  currentMode = MODE_TURN_ON;
  pendingMode = MODE_TURN_ON;
  wakeButtonPin = buttonPin;
  accelVddPin = accelVdd;
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

static void flashLED() {
  DisplayPower_showOn();
  isLEDOn = true;
  ledWakeTime = millis();
}

void DeviceStateManager_onEvent(DeviceEvent event) {
  switch (event) {
    case EVT_BUTTON_SHORT:
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
      BLEManager_sendEvent(EVT_MSG_NOT_WEARING);
      requestMode(MODE_IDLE);
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

void DeviceStateManager_loop() {
  if (pendingMode != currentMode) {
    exitMode(currentMode);
    currentMode = pendingMode;
    enterMode(currentMode);
  }

  if (currentMode == MODE_IDLE) idleLoop();
  else if (currentMode == MODE_SCREENING) AfibScreening_loop();
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

    default:
      break;
  }
}
