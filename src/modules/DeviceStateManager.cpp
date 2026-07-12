#include "DeviceStateManager.h"

static DeviceMode currentMode = MODE_IDLE;
static DeviceMode pendingMode = MODE_IDLE;
static unsigned long lastModeChangeTime = 0;
static unsigned long lastWearCheck = 0;
static unsigned long screeningStartTime = 0;
static unsigned long screeningEndTime = 0;
static unsigned long ledWakeTime = 0;
static bool isLEDOn = false;
static uint8_t wakeButtonPin;

static bool isWearing = false;
static long btnPressTime = 0;
static bool isBtnPressed = false;
static bool isLongPressHandled = false;
static bool isScreening = false;

static void enterMode(DeviceMode mode);
static void exitMode(DeviceMode mode);
static void DeviceStateManager_requestMode(DeviceMode newMode);

void DeviceStateManager_begin(uint8_t buttonPin) {
    currentMode = MODE_TURN_ON;
    pendingMode = MODE_TURN_ON;
    lastModeChangeTime = millis();
    lastWearCheck = 0;
    screeningStartTime = 0;
    screeningEndTime = 0;
    ledWakeTime = 0;
    isLEDOn = false;
    btnPressTime = 0;
    isBtnPressed = false;
    isLongPressHandled = false;
    isWearing = false;
    isScreening = false;
    wakeButtonPin = buttonPin;
    enterMode(MODE_TURN_ON);
}

void DeviceStateManager_handleButton() {
    if (digitalRead(wakeButtonPin) == LOW) {
        if (!isBtnPressed) {
            isBtnPressed = true;
            btnPressTime = millis();
            isLongPressHandled = false;
        } else if (!isLongPressHandled && (millis() - btnPressTime >= 5000)) {
            isLongPressHandled = true;
            DeviceStateManager_onEvent(EVT_BUTTON_LONG);
        }
    } else {
        if (isBtnPressed) {
            isBtnPressed = false;
            if (!isLongPressHandled) {
                DeviceStateManager_onEvent(EVT_BUTTON_SHORT);
            }
        }
    }

    if (isLEDOn && (millis() - ledWakeTime > 3000)) {
        DisplayPower_showOff();
        isLEDOn = false;
    }
}

void DeviceStateManager_onEvent(DeviceEvent event) {
    switch (event) {
        case EVT_BUTTON_SHORT:
            DisplayPower_showOn();
            isLEDOn = true;
            ledWakeTime = millis();
            Serial.println("event BUTTON_SHORT, LED on");
            break;

        case EVT_BUTTON_LONG:
            Serial.println("event BUTTON_LONG, device shutdown");
            DeviceStateManager_requestMode(MODE_SHUTDOWN);
            break;

        case EVT_MOTION:
            if (currentMode == MODE_MEASURE || currentMode == MODE_SCREENING) {
                Serial.print("[");
                Serial.print(millis());
                Serial.println("] event MOTION, detected arm motion");
                sendBLECommand("CMD:MOTION\n");
                if (currentMode == MODE_SCREENING) {
                    // Đang tầm soát mà nhúc nhích? Tắt đèn LED ngay lập tức 
                    // và ép nhảy vào chu kỳ nghỉ 30 giây để bỏ qua luồng data nhiễu.
                    isScreening = false; 
                    screeningEndTime = millis(); // Bắt đầu đếm 30s nghỉ
                    PPGManager_shutDown();
                } else {
                    // Nếu đang trong chế độ Đo chủ động (MEASURE), 
                    // chuyển thẳng về IDLE chờ người dùng thao tác lại
                    DeviceStateManager_requestMode(MODE_IDLE);
                }
            }
            break;

        case EVT_NOT_WEARING:
            Serial.println("event NOT_WEARING");
            DeviceStateManager_requestMode(MODE_IDLE);
            break;

        case EVT_BLE_START_MEASURE:
            Serial.println("event BLE_START_MEASURE");
            DeviceStateManager_requestMode(MODE_MEASURE);
            break;

        case EVT_BLE_START_WORKOUT:
            Serial.println("event BLE_START_WORKOUT");
            DeviceStateManager_requestMode(MODE_WORKOUT);
            break;

        case EVT_BLE_START_SCREENING:
            Serial.println("event BLE_START_SCREENING");
            DeviceStateManager_requestMode(MODE_SCREENING);
            break;
    }
}

void DeviceStateManager_loop() {
    if (pendingMode != currentMode) {
        exitMode(currentMode);
        currentMode = pendingMode;
        enterMode(currentMode);
        lastModeChangeTime = millis();
    }

    if (currentMode == MODE_IDLE) {
        if (millis() - lastWearCheck >= 3000) {
            lastWearCheck = millis();

            PPGManager_wakeUp();
            delay(50);

            long testIR = PPGManager_readIR();
            if (testIR > 50000) {
                Serial.println("[WEAR] Kich hoat do nhip tim!");
                DeviceStateManager_requestMode(MODE_SCREENING);
            } else {
                PPGManager_shutDown();
            }
        }
    }

    if (currentMode == MODE_SCREENING) {
        if (isScreening) {
            if (millis() - screeningStartTime >= 60000UL) {
                isScreening = false;
                screeningEndTime = millis();
                
                PPGManager_shutDown();
                Serial.println("[SCREENING] Xong 1 phut đo. Tam nghi 30 giay...");
            }
        } else {
            if (millis() - screeningEndTime >= 30000UL) {
                isScreening = true;
                screeningStartTime = millis();
                
                PPGManager_wakeUp();
                Serial.println("[SCREENING] Het 30 giay nghi. Bat dau đo lai 1 phut...");
            }
        }
    }
}

DeviceMode DeviceStateManager_getMode() {
    return currentMode;
}

static void DeviceStateManager_requestMode(DeviceMode newMode) {
    if (newMode == currentMode || newMode == pendingMode) {
        return;
    }
    pendingMode = newMode;
}

static void enterMode(DeviceMode mode) {
    switch (mode) {
        case MODE_IDLE:
            Serial.println("Mode IDLE");
            PPGManager_shutDown();
            AccelManager_setMotionThreshold(20);
            break;

        case MODE_MEASURE:
            Serial.println("Mode MEASURE");
            PPGManager_wakeUp();
            AccelManager_setMotionThreshold(5);
            break;

        case MODE_WORKOUT:
            Serial.println("Mode WORKOUT");
            PPGManager_wakeUp();
            AccelManager_setMotionThreshold(20);
            break;

        case MODE_SCREENING:
            Serial.println("Mode SCREENING");
            isScreening = true;
            screeningStartTime = millis();
            PPGManager_wakeUp();
            AccelManager_setMotionThreshold(10);
            break;

        case MODE_SHUTDOWN:
            Serial.println("Mode SHUTDOWN");
            PPGManager_shutDown();
            DisplayPower_showOff();
            pinMode(D4, INPUT);
            pinMode(D5, INPUT);
            delay(100);
            esp_deep_sleep_enable_gpio_wakeup(1ULL << wakeButtonPin, ESP_GPIO_WAKEUP_GPIO_LOW);
            esp_deep_sleep_start();
            break;
        case MODE_TURN_ON:
            Serial.println("Mode TURN_ON");
            DisplayPower_showOn();
            isLEDOn = true;
            ledWakeTime = millis();
            DeviceStateManager_requestMode(MODE_IDLE);
            break;
    }
}

static void exitMode(DeviceMode mode) {
    switch (mode) {
        case MODE_SCREENING:
            screeningStartTime = 0;
            break;

        case MODE_MEASURE:
        case MODE_WORKOUT:
        case MODE_IDLE:
        default:
            break;
    }
}