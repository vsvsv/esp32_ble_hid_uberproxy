#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "tinyusb.h"

// local includes
#include "ble_host.h"
#include "common.h"
#include "usb_hid.h"

// gpio number to report connection status (via blinking LED)
#define STATUS_LED_PIN GPIO_NUM_2

QueueHandle_t g_keyboard_queue = NULL;

static void led_status_task(void* pvParameters);

void app_main(void)
{
    // init BLE->USB mailbox queue
    g_keyboard_queue = xQueueCreate(32, sizeof(bhp_key_report_t));

    ESP_ERROR_CHECK(bhp_usb_initialize_as_keyboard());
    LOG_INFO("USB init ok. Waiting for connection...");

    ESP_ERROR_CHECK(bhp_ble_initialize());
    LOG_INFO("BLE init ok.");

    bhp_ble_start_scan(10, BHP_BLE_APPEARANCE_KEYBOARD);

    xTaskCreatePinnedToCore(led_status_task, "led_task", 2048, NULL, 1, NULL, 0);

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

static void led_status_task(void* pvParameters)
{
    gpio_reset_pin(STATUS_LED_PIN);
    gpio_set_direction(STATUS_LED_PIN, GPIO_MODE_OUTPUT);

    while (1) {
        if (g_is_ble_connected) {
            // solid color when connected
            gpio_set_level(STATUS_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            // blink when scanning/disconnected
            gpio_set_level(STATUS_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(STATUS_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
