#include "BLEManager.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>

static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool deviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      BLEDevice::startAdvertising();
    };
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
  pCharacteristic = pService->createCharacteristic(
                        "beb5483e-36e1-4688-b7f5-ea07361b26a8",
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pCharacteristic->addDescriptor(new BLE2902());
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

void sendBLECommand(const char* command){
  if (deviceConnected && pCharacteristic != NULL) {
    pCharacteristic->setValue((uint8_t*)command, strlen(command));
    pCharacteristic->notify();
  }
}