#ifndef BHP_BLE_HOST
#define BHP_BLE_HOST

#include "esp_err.h"

#define BHP_BLE_APPEARANCE_KEYBOARD 0x03C1
#define BHP_BLE_APPEARANCE_MOUSE 0x03C2

// initialize BLE controller and non-volatile storage
esp_err_t bhp_ble_initialize(void);

// start scanning for bluetooth devices
// can return `false` if the scan is already in-progress
bool bhp_ble_start_scan(uint32_t timeout_sec, uint16_t target_appearance);

#endif // BHP_BLE_HOST
