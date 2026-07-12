#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <stddef.h>

void BLEManager_begin();
bool BLEManager_isConnected();
void BLEManager_notify(const char* data, size_t len);
void sendBLECommand(const char* command);

#endif
