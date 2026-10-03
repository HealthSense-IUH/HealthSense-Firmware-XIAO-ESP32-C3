#include "BLEManager.h"
#include "BleProtocol.h"
#include "../config.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>

static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;        // NOTIFY: PPG thô + sự kiện
static BLECharacteristic* pReportCharacteristic = nullptr;  // NOTIFY: báo cáo vitals, dữ liệu offline
static BLECharacteristic* pWriteCharacteristic = nullptr;   // WRITE: lệnh từ điện thoại
static BLECharacteristic* pBatteryCharacteristic = nullptr; // READ | NOTIFY: mức pin (0x2A19)

// Đổi trong callback của BLE stack (task khác) nên phải volatile
static volatile bool deviceConnected = false;
static volatile unsigned long connectedAt = 0;

// Hàm xử lý lệnh từ điện thoại, do DeviceStateManager đăng ký
static void (*commandCallback)(const char* cmd) = nullptr;

// ---- Pin ----

uint8_t BLEManager_readBatteryLevel() {
  // Cầu phân áp 2 x 100K: điện áp pin = 2 x điện áp tại chân ADC
  uint32_t batteryMilliVolts = analogReadMilliVolts(BATTERY_ADC_PIN) * 2;
  // Do suy hao mạch / sai số trở, pin đầy đo được ~4100mV
  if (batteryMilliVolts >= 4100) return 100;
  if (batteryMilliVolts <= 3300) return 0;
  return (uint8_t)(((batteryMilliVolts - 3300) * 100) / (4100 - 3300));
}

void BLEManager_updateBatteryLevel() {
  if (pBatteryCharacteristic == nullptr) return;
  uint8_t level = BLEManager_readBatteryLevel();
  pBatteryCharacteristic->setValue(&level, 1);
  if (deviceConnected) pBatteryCharacteristic->notify();
}

// ---- Bộ đệm offline: mẫu tập luyện ghi lại khi mất kết nối, gửi bù khi kết nối lại ----

struct OfflineSample {
  uint32_t timestampMs;
  uint8_t bpm;
  uint32_t steps;
};

#define MAX_OFFLINE_SAMPLES 1200        // 1 giờ ở nhịp 3 giây / mẫu
#define RESYNC_START_DELAY_MS 1000UL    // Chờ điện thoại bật notify sau khi kết nối rồi mới gửi bù
#define RESYNC_INTERVAL_MS 35UL         // Giãn cách giữa các gói gửi bù để không ngập BLE
static OfflineSample offlineRingBuffer[MAX_OFFLINE_SAMPLES];
static uint16_t offlineHead = 0;
static uint16_t offlineTail = 0;
static uint16_t offlineCount = 0;
static bool resyncing = false;
static unsigned long lastResyncSend = 0;

void BLEManager_pushOfflineSample(uint8_t bpm, uint32_t steps) {
  if (deviceConnected) return;

  offlineRingBuffer[offlineHead] = {(uint32_t)millis(), bpm, steps};
  offlineHead = (offlineHead + 1) % MAX_OFFLINE_SAMPLES;
  if (offlineCount < MAX_OFFLINE_SAMPLES) {
    offlineCount++;
  } else {
    // Đầy: đè mẫu cũ nhất
    offlineTail = (offlineTail + 1) % MAX_OFFLINE_SAMPLES;
  }
}

/** Gửi bù tối đa một mẫu offline mỗi lần gọi; gọi từ loop() nên không chặn BLE stack. */
static void resyncOfflineSamples() {
  if (!deviceConnected || offlineCount == 0 || pReportCharacteristic == nullptr) {
    resyncing = false;
    return;
  }
  unsigned long now = millis();
  if (now - connectedAt < RESYNC_START_DELAY_MS || now - lastResyncSend < RESYNC_INTERVAL_MS) return;

  if (!resyncing) {
    resyncing = true;
    Serial.print("[BLE] Đang nhả bù dữ liệu offline: ");
    Serial.print(offlineCount);
    Serial.println(" mẫu");
  }

  OfflineSample sample = offlineRingBuffer[offlineTail];
  offlineTail = (offlineTail + 1) % MAX_OFFLINE_SAMPLES;
  offlineCount--;

  char resyncMsg[64];
  int len = snprintf(resyncMsg, sizeof(resyncMsg), FMT_RESYNC, (unsigned long)sample.timestampMs,
                     (unsigned int)sample.bpm, (unsigned long)sample.steps);
  pReportCharacteristic->setValue((uint8_t*)resyncMsg, len);
  pReportCharacteristic->notify();
  lastResyncSend = now;
}

// ---- Callback BLE ----

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) {
    connectedAt = millis();
    deviceConnected = true;
  }
  void onDisconnect(BLEServer*) {
    deviceConnected = false;
    BLEDevice::startAdvertising();
  }
};

// Điện thoại ghi lệnh vào characteristic WRITE
class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    std::string raw = pChar->getValue();
    if (raw.empty() || commandCallback == nullptr) return;

    // Bỏ '\n', '\r', khoảng trắng cuối
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' '))
      raw.pop_back();

    Serial.print("[BLE-RX] Nhận lệnh từ điện thoại: ");
    Serial.println(raw.c_str());
    commandCallback(raw.c_str());
  }
};

void BLEManager_begin() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char deviceName[20];
  snprintf(deviceName, sizeof(deviceName), BLE_DEVICE_NAME_PREFIX "%02X%02X", mac[4], mac[5]);

  BLEDevice::init(deviceName);
  BLEDevice::setMTU(512);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(BLE_DATA_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());

  pWriteCharacteristic = pService->createCharacteristic(
      BLE_COMMAND_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pWriteCharacteristic->setCallbacks(new WriteCallbacks());

  pReportCharacteristic = pService->createCharacteristic(BLE_REPORT_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pReportCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  // Battery Service chuẩn Bluetooth SIG
  BLEService* pBatteryService = pServer->createService(BLEUUID((uint16_t)BLE_BATTERY_SERVICE_UUID));
  pBatteryCharacteristic = pBatteryService->createCharacteristic(
      BLEUUID((uint16_t)BLE_BATTERY_LEVEL_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pBatteryCharacteristic->addDescriptor(new BLE2902());
  uint8_t initBatteryLevel = BLEManager_readBatteryLevel();
  pBatteryCharacteristic->setValue(&initBatteryLevel, 1);
  pBatteryService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->addServiceUUID(BLEUUID((uint16_t)BLE_BATTERY_SERVICE_UUID));
  pAdvertising->setScanResponse(false);
  BLEDevice::startAdvertising();
}

void BLEManager_loop() {
  resyncOfflineSamples();
}

bool BLEManager_isConnected() {
  return deviceConnected;
}

static void notify(BLECharacteristic* characteristic, const char* data, size_t len) {
  if (characteristic == nullptr) return;
  characteristic->setValue((uint8_t*)data, len);
  characteristic->notify();
}

void BLEManager_notify(const char* data, size_t len) {
  notify(pCharacteristic, data, len);
}

void BLEManager_notifyReport(const char* data, size_t len) {
  notify(pReportCharacteristic, data, len);
}

void BLEManager_sendEvent(const char* message) {
  if (deviceConnected) notify(pCharacteristic, message, strlen(message));
}

void BLEManager_startAdvertising() {
  BLEDevice::startAdvertising();
}

void BLEManager_stopAdvertising() {
  BLEDevice::getAdvertising()->stop();
}

void BLEManager_setCommandCallback(void (*callback)(const char* cmd)) {
  commandCallback = callback;
}
