#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "tinyusb.h"

// local includes
#include "ble_host.h"
#include "common.h"
#include "usb_hid.h"

QueueHandle_t g_keyboard_queue = NULL;

void app_main(void)
{
    // init BLE->USB mailbox queue
    g_keyboard_queue = xQueueCreate(32, sizeof(bhp_key_report_t));

    ESP_ERROR_CHECK(bhp_usb_initialize_as_keyboard());
    LOG_INFO("USB init ok. Waiting for connection...");

    ESP_ERROR_CHECK(bhp_ble_initialize());
    LOG_INFO("BLE init ok.");

    bhp_ble_start_scan(10, BHP_BLE_APPEARANCE_KEYBOARD);

    bhp_key_report_t report;
    while (1) {
        // drain the queue and send events to USB
        if (xQueueReceive(g_keyboard_queue, &report, portMAX_DELAY) == pdTRUE) {
            if (!tud_mounted()) {
                continue;
            }

            int timeout = 0;
            while (!tud_hid_ready() && timeout < 10) {
                esp_rom_delay_us(100);
                timeout++;
            }

            // if it is somehow still busy after 1ms, yield to RTOS
            while (!tud_hid_ready()) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            if (report.report_id == BHP_REPORT_TYPE_KEYBOARD) {
                tud_hid_keyboard_report(1, report.modifier, report.keycode);
            } else if (report.report_id == BHP_REPORT_TYPE_CONSUMER) {
                // media key is pressed
                tud_hid_report(2, &report.consumer_key, sizeof(report.consumer_key));
            }
        }
    }
}
