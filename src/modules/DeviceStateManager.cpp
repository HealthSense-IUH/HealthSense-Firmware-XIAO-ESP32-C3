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
static uint8_t accelVddPin_stored;

static bool isWearing = false;
static long btnPressTime = 0;
static bool isBtnPressed = false;
static bool isLongPressHandled = false;
static bool isScreening = false;

// Screening cycle management (10-minute cycle with 2 phases)
static unsigned long screeningCycleStart = 0;
// 0: idle, 1: Phase 1 (1 min measure), 2: Phase 1 sleep dynamic, 3: Phase 2 (30s measure), 4: Phase 2 sleep dynamic, 5: Phase 1 micro-retry wait (30s)
static uint8_t screeningRetryCount = 0;
static uint8_t screeningPhase = 0;
static unsigned long phaseStartTime = 0;
static unsigned long retrySleepStart = 0;
static unsigned long phaseSleepStart = 0;
static unsigned long phaseSleepDuration = 0;
static bool isSpo2Missed = false;
static uint8_t phase1AvgSpO2 = 0;

// Accumulators for averaging vitals
static unsigned long bpmSum = 0;
static unsigned long spo2Sum = 0;
static unsigned long validSampleCount = 0;
static unsigned long lastSampleAccumTime = 0;

static void enterMode(DeviceMode mode);
static void exitMode(DeviceMode mode);
static void DeviceStateManager_requestMode(DeviceMode newMode);

// Hàm xử lý lệnh nhận từ điện thoại qua BLE Write
// Được đăng ký vào BLEManager trong begin()
static void onBLECommand(const char* cmd) {
    if (strcmp(cmd, "CMD:START_MEASURE") == 0) {
        DeviceStateManager_onEvent(EVT_BLE_START_MEASURE);
    } else if (strcmp(cmd, "CMD:START_WORKOUT") == 0) {
        DeviceStateManager_onEvent(EVT_BLE_START_WORKOUT);
    } else if (strcmp(cmd, "CMD:START_SCREENING") == 0) {
        DeviceStateManager_onEvent(EVT_BLE_START_SCREENING);
    } else if (strcmp(cmd, "CMD:IDLE") == 0) {
        DeviceStateManager_onEvent(EVT_NOT_WEARING); // Về IDLE
    } else {
        Serial.print("[BLE-RX] Lệnh không xác định: ");
        Serial.println(cmd);
    }
}

void DeviceStateManager_begin(uint8_t buttonPin, uint8_t accelVddPin) {
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
    accelVddPin_stored = accelVddPin;
    // Đăng ký handler lệnh BLE Write
    BLEManager_setCommandCallback(onBLECommand);
    enterMode(MODE_TURN_ON);
}

void DeviceStateManager_handleButton() {
    if (digitalRead(wakeButtonPin) == LOW) {
        if (!isBtnPressed) {
            isBtnPressed = true;
            btnPressTime = millis();
            isLongPressHandled = false;
        } else if (!isLongPressHandled && (millis() - btnPressTime >= 4000)) {
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
                    if (screeningPhase == 1) {
                        Serial.println("[SCREENING] Phát hiện chuyển động! Ngừng đo Pha 1 và thử lại chớp nhoáng...");
                        
                        // Cứu vớt dữ liệu: Nếu đã thu thập được > 10 mẫu hợp lệ trước khi bị rung tay
                        if (validSampleCount > 10) {
                            uint8_t avgBPM = bpmSum / validSampleCount;
                            uint8_t avgSpO2 = spo2Sum / validSampleCount;
                            char report[32];
                            snprintf(report, sizeof(report), "R1:%u,%u\n", avgBPM, avgSpO2);
                            if (BLEManager_isConnected()) {
                                BLEManager_notifyReport(report, strlen(report));
                                Serial.print("[SCREENING] Đã vớt vát dữ liệu sinh hiệu trước khi hủy: ");
                                Serial.print(avgBPM);
                                Serial.print(" BPM, ");
                                Serial.print(avgSpO2);
                                Serial.println("%");
                            }
                        }
                        
                        PPGManager_shutDown(); // Tắt cảm biến ngay lập tức
                        screeningRetryCount++;
                        
                        if (screeningRetryCount < 3) {
                            // Đi ngủ chớp nhoáng 30 giây
                            screeningPhase = 9;
                            retrySleepStart = millis();
                            Serial.print("[SCREENING] Micro-Retry lần ");
                            Serial.print(screeningRetryCount);
                            Serial.println("/3. Ngủ đông 30 giây...");
                        } else {
                            // Thất bại cả 3 lần
                            isSpo2Missed = true;
                            Serial.println("[SCREENING] Thất bại 3 lần đo Pha 1. Bỏ lỡ SpO2. Nghỉ đến phút thứ 5...");
                            
                            // Chuyển sang pha nghỉ chờ Pha 2
                            screeningPhase = 2;
                            phaseSleepStart = millis();
                            // Tính toán thời gian ngủ động để đến mốc phút tiếp theo
                            long targetTime = screeningCycleStart + 150 * 1000;
                            long remaining = targetTime - millis();
                            phaseSleepDuration = remaining > 0 ? remaining : 0;
                        }
                    }
                } else {
                    // Nếu đang trong chế độ Đo chủ động (MEASURE), 
                    // chuyển thẳng về IDLE chờ người dùng thao tác lại
                    DeviceStateManager_requestMode(MODE_IDLE);
                }
            }
            break;

        case EVT_NOT_WEARING:
            Serial.println("event NOT_WEARING");
            sendBLECommand("CMD:NOT_WEARING\n");
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
            sendBLECommand("CMD:START_SCREENING\n");
            if (currentMode == MODE_SCREENING) {
                // Ép khởi động lại chu kỳ nếu đang ở trong chế độ chờ/ngủ của Screening
                exitMode(MODE_SCREENING);
                enterMode(MODE_SCREENING);
            } else {
                DeviceStateManager_requestMode(MODE_SCREENING);
            }
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
        unsigned long now = millis();
        
        if (screeningPhase == 1) {
            // ĐANG ĐO PHA 1 (1 phút)
            // Tích luỹ BPM/SpO2 mỗi 1 giây
            if (now - lastSampleAccumTime >= 1000) {
                lastSampleAccumTime = now;
                uint8_t bpm = PPGManager_getBPM();
                uint8_t spo2 = PPGManager_getSpO2();
                if (bpm > 0 && spo2 > 0) { // Chỉ lấy dữ liệu khi hợp lệ
                    bpmSum += bpm;
                    spo2Sum += spo2;
                    validSampleCount++;
                }
            }
            
            // Đã đo đủ 1 phút liên tục
            if (now - phaseStartTime >= 60000UL) {
                PPGManager_shutDown();
                
                uint8_t avgBPM = 0;
                uint8_t avgSpO2 = 0;
                if (validSampleCount > 0) {
                    avgBPM = bpmSum / validSampleCount;
                    avgSpO2 = spo2Sum / validSampleCount;
                }
                
                phase1AvgSpO2 = avgSpO2; // Lưu lại để dùng ở Pha 2
                isSpo2Missed = false;
                
                Serial.print("[SCREENING] Pha 1 thành công! Avg BPM: ");
                Serial.print(avgBPM);
                Serial.print(" | Avg SpO2: ");
                Serial.println(avgSpO2);
                
                // Gửi qua Characteristic 2 (Báo cáo Pha 1 hoàn tất)
                if (BLEManager_isConnected()) {
                    char report[32];
                    snprintf(report, sizeof(report), "R1:%u,%u\n", avgBPM, avgSpO2);
                    BLEManager_notifyReport(report, strlen(report));
                }
                
                // Chuyển sang pha nghỉ chờ Pha 2 (mốc 150s = 2.5 phút)
                screeningPhase = 2;
                phaseSleepStart = millis();
                long targetTime = screeningCycleStart + 150 * 1000UL;
                long remaining = targetTime - (long)millis();
                phaseSleepDuration = remaining > 0 ? remaining : 0;
                
                Serial.print("[SCREENING] Ngủ động ");
                Serial.print(phaseSleepDuration / 1000);
                Serial.println(" giây để đến mốc 2.5 phút (Pha 2)...");
            }
        } 
        else if (screeningPhase == 9) {
            // ĐANG NGỦ MICRO-RETRY (30 giây)
            if (now - retrySleepStart >= 30000UL) {
                long timeRemainingForP1 = (long)(screeningCycleStart + 150000UL) - (long)now;
                if (timeRemainingForP1 < 60000L) {
                    // Thời gian còn lại đến mốc Pha 2 < 60s, không đủ để đo trọn vẹn 1 phút Pha 1
                    Serial.println("[SCREENING] Thời gian còn lại < 60s, không đủ đo trọn vẹn Pha 1. Hủy đo Pha 1 và chuyển sang chờ Pha 2...");
                    isSpo2Missed = true;
                    screeningPhase = 2;
                    phaseSleepStart = millis();
                    phaseSleepDuration = timeRemainingForP1 > 0 ? timeRemainingForP1 : 0;
                } else {
                    // Đủ thời gian (>= 60s) -> Thức dậy đo lại Pha 1
                    Serial.println("[SCREENING] Hết 30 giây ngủ micro-retry. Thức dậy đo lại Pha 1...");
                    screeningPhase = 1;
                    phaseStartTime = millis();
                    bpmSum = 0;
                    spo2Sum = 0;
                    validSampleCount = 0;
                    lastSampleAccumTime = millis();
                    
                    PPGManager_wakeUp();
                    PPGManager_setupPhase1();
                }
            }
        }
        else if (screeningPhase == 2 || screeningPhase == 4 || screeningPhase == 6) {
            // ĐANG NGHỈ CHỜ PHA TIẾP THEO
            if (now - phaseSleepStart >= phaseSleepDuration) {
                uint8_t nextMeasurePhase = 3;
                if (screeningPhase == 2) nextMeasurePhase = 3;      // Pha 2 (mốc 2.5 min)
                else if (screeningPhase == 4) nextMeasurePhase = 5; // Pha 3 (mốc 5.0 min)
                else if (screeningPhase == 6) nextMeasurePhase = 7; // Pha 4 (mốc 7.5 min)

                screeningPhase = nextMeasurePhase;
                phaseStartTime = millis();
                bpmSum = 0;
                spo2Sum = 0;
                validSampleCount = 0;
                lastSampleAccumTime = millis();
                
                Serial.print("[SCREENING] Thức dậy bắt đầu Pha đo thứ ");
                Serial.print((screeningPhase / 2) + 1);
                Serial.println("...");

                PPGManager_wakeUp();
                PPGManager_setupPhase2(isSpo2Missed == false); // lowPower = true nếu không bị lỡ SpO2
            }
        }
        else if (screeningPhase == 3 || screeningPhase == 5 || screeningPhase == 7) {
            // ĐANG ĐO PHA PHỤ (Pha 2, 3, 4 - 30 giây)
            // Tích luỹ BPM/SpO2 mỗi 1 giây
            if (now - lastSampleAccumTime >= 1000) {
                lastSampleAccumTime = now;
                uint8_t bpm = PPGManager_getBPM();
                uint8_t spo2 = PPGManager_getSpO2();
                if (bpm > 0) {
                    bpmSum += bpm;
                    if (isSpo2Missed && spo2 > 0) {
                        spo2Sum += spo2;
                    }
                    validSampleCount++;
                }
            }
            
            // Đã đo đủ 30 giây
            if (now - phaseStartTime >= 30000UL) {
                PPGManager_shutDown();
                
                uint8_t avgBPM = 0;
                uint8_t avgSpO2 = 0;
                if (validSampleCount > 0) {
                    avgBPM = bpmSum / validSampleCount;
                    if (isSpo2Missed) {
                        avgSpO2 = spo2Sum / validSampleCount;
                    }
                }
                
                // Lọc BPM trước khi gửi
                uint8_t finalBPMReport = 0;
                if (avgBPM > 40 && avgBPM < 255) {
                    finalBPMReport = avgBPM;
                } else {
                    finalBPMReport = 0;
                }
                
                // Xác định SpO2 gửi đi
                uint8_t finalSpO2Report = isSpo2Missed ? avgSpO2 : phase1AvgSpO2;
                uint8_t phaseNumber = (screeningPhase / 2) + 1;
                
                Serial.print("[SCREENING] Pha ");
                Serial.print(phaseNumber);
                Serial.print(" hoàn tất! Avg BPM: ");
                Serial.print(avgBPM);
                Serial.print(" (Gửi: ");
                Serial.print(finalBPMReport);
                Serial.print(") | SpO2: ");
                Serial.println(finalSpO2Report);
                
                // Gửi qua Characteristic 2 (Báo cáo trung bình các pha 2, 3, 4)
                if (BLEManager_isConnected()) {
                    char report[32];
                    snprintf(report, sizeof(report), "R2:%u,%u\n", finalBPMReport, finalSpO2Report);
                    BLEManager_notifyReport(report, strlen(report));
                }
                
                // Tính toán mốc mục tiêu cho pha tiếp theo
                uint8_t nextSleepPhase = 4;
                unsigned long targetMs = 300000UL; // Mốc mặc định Pha 2 -> 3 (300s = 5 min)

                if (screeningPhase == 3) {
                    nextSleepPhase = 4;
                    targetMs = 300000UL; // 5.0 phút
                } else if (screeningPhase == 5) {
                    nextSleepPhase = 6;
                    targetMs = 450000UL; // 7.5 phút
                } else if (screeningPhase == 7) {
                    nextSleepPhase = 8;
                    targetMs = 600000UL; // 10.0 phút (Kết thúc chu kỳ)
                }

                screeningPhase = nextSleepPhase;
                phaseSleepStart = millis();
                long targetTime = screeningCycleStart + targetMs;
                long remaining = targetTime - (long)millis();
                phaseSleepDuration = remaining > 0 ? remaining : 0;
                
                Serial.print("[SCREENING] Ngủ động ");
                Serial.print(phaseSleepDuration / 1000);
                Serial.print(" giây để đến mốc ");
                Serial.print(targetMs / 1000);
                Serial.println("s...");
            }
        }
        else if (screeningPhase == 8) {
            // ĐANG NGHỈ KẾT THÚC CHU KỲ (đến mốc 600s = 10 phút) -> Bắt đầu lại chu kỳ 10 phút mới
            if (now - phaseSleepStart >= phaseSleepDuration) {
                Serial.println("[SCREENING] Đã xong 10 phút chu kỳ. Bắt đầu chu kỳ 10 phút mới (Pha 1)...");
                screeningCycleStart = millis();
                screeningPhase = 1;
                phaseStartTime = millis();
                bpmSum = 0;
                spo2Sum = 0;
                validSampleCount = 0;
                screeningRetryCount = 0;
                isSpo2Missed = false;
                lastSampleAccumTime = millis();
                
                PPGManager_wakeUp();
                PPGManager_setupPhase1();
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
            // Giữ BLE advertising bật khi IDLE để điện thoại luôn tìm thấy và chủ động kết nối được
            if (!BLEManager_isConnected()) {
                BLEManager_startAdvertising();
                Serial.println("[BLE] Đang bật advertising (IDLE)");
            }
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
            screeningCycleStart = millis();
            screeningPhase = 1; // Bắt đầu Pha 1
            screeningRetryCount = 0;
            isSpo2Missed = false;
            phase1AvgSpO2 = 0;
            phaseStartTime = millis();
            
            bpmSum = 0;
            spo2Sum = 0;
            validSampleCount = 0;
            lastSampleAccumTime = millis();
            
            PPGManager_wakeUp();
            PPGManager_setupPhase1();
            AccelManager_setMotionThreshold(8);
            break;

        case MODE_SHUTDOWN:
            Serial.println("Mode SHUTDOWN");
            PPGManager_shutDown();
            DisplayPower_showRed();   // Báo hiệu đang tắt (đỏ 1 giây)
            delay(1000);
            DisplayPower_showOff();
            // Tắt nguồn MPU6050 để tiết kiệm pin khi deep sleep
            digitalWrite(accelVddPin_stored, LOW);
            Serial.println("[POWER] Đã tắt nguồn MPU6050");
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
            screeningPhase = 0;
            break;

        case MODE_IDLE:
            // Bật lại BLE advertising khi thoát IDLE (bắt đầu đo)
            BLEManager_startAdvertising();
            Serial.println("[BLE] Đã bật advertising (thoát IDLE)");
            break;

        case MODE_MEASURE:
        case MODE_WORKOUT:
        default:
            break;
    }
}