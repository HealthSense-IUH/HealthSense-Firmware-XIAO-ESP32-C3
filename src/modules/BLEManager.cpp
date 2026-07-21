#include "BLEManager.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>

static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;      // NOTIFY (device -> phone) raw PPG
static BLECharacteristic* pReportCharacteristic = nullptr;  // NOTIFY (device -> phone) report vitals
static BLECharacteristic* pWriteCharacteristic = nullptr;  // WRITE  (phone -> device)
static bool deviceConnected = false;

// Con trỏ tới hàm xử lý lệnh nhận từ điện thoại, được đăng ký bởi DeviceStateManager
static void (*commandCallback)(const char* cmd) = nullptr;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      BLEDevice::startAdvertising();
    };
};

// Callback được gọi khi điện thoại Write lệnh vào thiết bị
class WriteCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) {
      std::string raw = pChar->getValue();
      if (raw.empty() || commandCallback == nullptr) return;

      // Chuẩn hóa: xóa '\n', '\r', khoảng trắng thừa
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
  sprintf(deviceName, "HuyWatch_%02X%02X", mac[4], mac[5]);

  BLEDevice::init(deviceName);
  BLEDevice::setMTU(512);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

  // Characteristic NOTIFY: thiết bị gửi dữ liệu lên điện thoại
  pCharacteristic = pService->createCharacteristic(
                        "beb5483e-36e1-4688-b7f5-ea07361b26a8",
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pCharacteristic->addDescriptor(new BLE2902());

  // Characteristic WRITE: điện thoại gửi lệnh xuống thiết bị
  pWriteCharacteristic = pService->createCharacteristic(
                        "beb5483e-36e1-4688-b7f5-ea07361b26a9",  // UUID kế tiếp
                        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                      );
  pWriteCharacteristic->setCallbacks(new WriteCallbacks());

  // Characteristic REPORT NOTIFY: thiết bị gửi bpm/spo2 trung bình
  pReportCharacteristic = pService->createCharacteristic(
                        "beb5483e-36e1-4688-b7f5-ea07361b26aa",  // UUID kế tiếp nữa
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pReportCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
  pAdvertising->setScanResponse(false);
  BLEDevice::startAdvertising();
}

bool BLEManager_isConnected() {
  return deviceConnected;
}

void BLEManager_notify(const char* data, size_t len) {
  if (!pCharacteristic) return;
  pCharacteristic->setValue((uint8_t*)data, len);
  pCharacteristic->notify();
}

void BLEManager_notifyReport(const char* data, size_t len) {
  if (!pReportCharacteristic) return;
  pReportCharacteristic->setValue((uint8_t*)data, len);
  pReportCharacteristic->notify();
}

void sendBLECommand(const char* command){
  if (deviceConnected && pCharacteristic != NULL) {
    pCharacteristic->setValue((uint8_t*)command, strlen(command));
    pCharacteristic->notify();
  }
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