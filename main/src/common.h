#ifndef BHP_COMMON
#define BHP_COMMON

// comment this out to disable verbose logging
#define BHP_ENABLE_VERBOSE_LOGS

#define ALIGN(X) __attribute__((aligned(X)))

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char* BHP_DEVICE_TAG = "[BLE HID UberProxy]";

#ifdef BHP_ENABLE_VERBOSE_LOGS
    #define LOG_DEBUG(format, ...) ESP_LOGI(BHP_DEVICE_TAG, format, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(format, ...) do {} while(0)
#endif

#define LOG_INFO(format, ...) ESP_LOGI(BHP_DEVICE_TAG, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) ESP_LOGE(BHP_DEVICE_TAG, format, ##__VA_ARGS__)

typedef enum {
    BHP_REPORT_TYPE_KEYBOARD = 1,
    BHP_REPORT_TYPE_CONSUMER = 2
} bhp_report_type_t;

// represents one keyboard event
typedef struct {
bhp_report_type_t report_id;
    uint8_t modifier;
    uint8_t keycode[6];
    uint16_t consumer_key;
} bhp_key_report_t;

// RTOS queue to connect BLE input to USB bus output
extern QueueHandle_t g_keyboard_queue;

extern bool g_is_ble_connected;

#endif // BHP_COMMON
