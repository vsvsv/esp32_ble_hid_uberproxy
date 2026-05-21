#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "nvs_flash.h"

// local includes
#include "ble_host.h"
#include "common.h"

static bool s_is_scanning = false;
static uint32_t s_scan_timeout_sec = 0;
static uint16_t s_target_appearance = 0;

static bool s_target_found = false;
static esp_bd_addr_t s_target_bda;
static esp_ble_addr_type_t s_target_addr_type;

bool g_is_ble_connected = false;

esp_err_t bhp_ble_setup_security(void);
static void init_hid_connection_in_separate_task(void*);
static bool check_device_is_already_bonded(struct ble_scan_result_evt_param* scan_result);

// callback for bluetooth events
static void gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: { // some scan-related event
            struct ble_scan_result_evt_param* scan_result = &param->scan_rst;
            switch (scan_result->search_evt) {
                case ESP_GAP_SEARCH_INQ_RES_EVT: { // new device discovered
                    bool is_already_bonded = check_device_is_already_bonded(scan_result);

                    uint16_t appearance = 0;
                    { // extract "appearance" (device kind)
                        uint8_t app_len = 0;
                        uint8_t* app_d = esp_ble_resolve_adv_data_by_type(
                            scan_result->ble_adv,
                            scan_result->adv_data_len + scan_result->scan_rsp_len,
                            ESP_BLE_AD_TYPE_APPEARANCE,
                            &app_len
                        );

                        if (app_d && app_len >= 2) {
                            appearance = app_d[0] | (app_d[1] << 8);
                        }
                    }

                    char name[64] = { 0 };
                    { // extract device name
                        uint8_t name_len = 0;
                        uint8_t* name_d = esp_ble_resolve_adv_data_by_type(
                            scan_result->ble_adv,
                            scan_result->adv_data_len + scan_result->scan_rsp_len,
                            ESP_BLE_AD_TYPE_NAME_CMPL,
                            &name_len
                        );

                        if (name_d && name_len > 0) {
                            size_t copy =
                                (name_len < sizeof(name) - 1) ? name_len : sizeof(name) - 1;
                            memcpy(name, name_d, copy);
                            name[copy] = '\0';
                        }
                    }

                    if (s_target_found) {
                        break;
                    }
                    if (appearance == s_target_appearance || is_already_bonded) {
                        s_target_found = true;
                        LOG_INFO(
                            "Found BLE keyboard! MAC: %02x:%02x:%02x:%02x:%02x:%02x, Name: '%s', "
                            "RSSI: "
                            "%d",
                            scan_result->bda[0],
                            scan_result->bda[1],
                            scan_result->bda[2],
                            scan_result->bda[3],
                            scan_result->bda[4],
                            scan_result->bda[5],
                            name,
                            scan_result->rssi
                        );

                        memcpy(s_target_bda, scan_result->bda, sizeof(esp_bd_addr_t));
                        s_target_addr_type = scan_result->ble_addr_type;

                        // stop scanning
                        esp_ble_gap_stop_scanning();
                        s_is_scanning = false;

                        xTaskCreate(
                            init_hid_connection_in_separate_task,
                            "hid_connect_task",
                            4096,
                            NULL,
                            5,
                            NULL
                        );
                    }
                    break;
                }
                case ESP_GAP_SEARCH_INQ_CMPL_EVT: { // scan completed
                    s_is_scanning = false;
                    LOG_INFO("No HID devices found. Restarting scan...");
                    bhp_ble_start_scan(s_scan_timeout_sec, s_target_appearance);
                    break;
                }
                default:
                    break;
            }

            // filter only for "discovered" events
            if (scan_result->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {}
            break;
        }
        case ESP_GAP_BLE_SEC_REQ_EVT: { // new device asks to pair
            LOG_DEBUG("Security requested by device, accepting...");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
        }
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: { // a passcode need to be typed on the BLE device
            LOG_INFO("");
            LOG_INFO("  ┌────────────────────────────────────────────────────┐");
            LOG_INFO("  │          ENTER PASSCODE TO PAIR THE DEVICE         │");
            LOG_INFO("  ├────────────────────────────────────────────────────┤");
            LOG_INFO("  │   PAIRING CODE: %06" PRIu32 "                      │", param->ble_security.key_notif.passkey);
            LOG_INFO("  │   Type this on the BLE Keyboard and press ENTER!   │");
            LOG_INFO("  └────────────────────────────────────────────────────┘");
            LOG_INFO("");
            break;
        }
        case ESP_GAP_BLE_AUTH_CMPL_EVT: { // pairing process ended
            if (param->ble_security.auth_cmpl.success) {
                LOG_INFO("Pairing SUCCESS! Device bonded to the ESP32.");
            } else {
                LOG_ERROR(
                    "Pairing FAILED! Reason: 0x%x", param->ble_security.auth_cmpl.fail_reason
                );
            }
            break;
        }
        default:
            break;
    }
}

static bool check_device_is_already_bonded(struct ble_scan_result_evt_param* scan_result)
{
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num == 0) {
        return false;
    }
    esp_ble_bond_dev_t* dev_list = malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
    if (!dev_list) {
        return false;
    }
    esp_ble_get_bond_device_list(&dev_num, dev_list);
    for (int i = 0; i < dev_num; i++) {
        if (memcmp(scan_result->bda, dev_list[i].bd_addr, sizeof(esp_bd_addr_t)) == 0) {
            return true;
        }
    }
    free(dev_list);
    return false;
}

static void init_hid_connection_in_separate_task(void* params)
{
    LOG_INFO("Initiating HID device connection...");
    esp_hidh_dev_open(s_target_bda, ESP_HID_TRANSPORT_BLE, s_target_addr_type);
    vTaskDelete(NULL); // kill task
}

// HID host event callback, will catch the events once the device is paired
static void hidh_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t* p = (esp_hidh_event_data_t*)event_data;

    switch (event) {
        case ESP_HIDH_OPEN_EVENT: {
            if (p->open.status == ESP_OK) {
                g_is_ble_connected = true;

                const uint8_t* bda = esp_hidh_dev_bda_get(p->open.dev);
                LOG_DEBUG(
                    "HID connection OPENED to MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                    bda[0],
                    bda[1],
                    bda[2],
                    bda[3],
                    bda[4],
                    bda[5]
                );
            } else {
                LOG_ERROR("HID connection FAILED. Status: %d. Restarting scan...", p->open.status);
                s_target_found = false;
                bhp_ble_start_scan(s_scan_timeout_sec, s_target_appearance);
            }
            break;
        }
        case ESP_HIDH_INPUT_EVENT: { // keystrokes, mouse clicks, etc
            if (g_keyboard_queue == NULL) {
                break;
            }
            if (p->input.usage == ESP_HID_USAGE_KEYBOARD && p->input.length >= 2) {
                bhp_key_report_t report = { 0 };
                report.report_id = BHP_REPORT_TYPE_KEYBOARD;

                // extract modifier (shift, ctrl, etc)
                report.modifier = p->input.data[0];

                // Copy up to 6 keys to match the standard USB HID specification
                int num_keys = (p->input.length - 1 > 6) ? 6 : (p->input.length - 1);
                memcpy(report.keycode, &p->input.data[1], num_keys);

                // push new events to the RTOS BLE->USB queue
                xQueueSend(g_keyboard_queue, &report, 0);
            } else if (p->input.usage == ESP_HID_USAGE_CCONTROL && p->input.length >= 3) {
                bhp_key_report_t report = { 0 };
                report.report_id = BHP_REPORT_TYPE_CONSUMER;

                report.consumer_key = p->input.data[0] | (p->input.data[1] << 8);

                xQueueSend(g_keyboard_queue, &report, 0);
            }
            break;
        }
        case ESP_HIDH_CLOSE_EVENT: {
            g_is_ble_connected = false;

            LOG_INFO("HID connection closed. Starting BLE scan again...");
            s_target_found = false;
            bhp_ble_start_scan(s_scan_timeout_sec, s_target_appearance);
            break;
        }
        default:
            break;
    }
}

esp_err_t bhp_ble_initialize(void)
{
    esp_log_level_set("BT_BTM", ESP_LOG_ERROR);

    // init non-volatile storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    LOG_DEBUG("non-volatile storage initialized");

    // init bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)
    ); // disable old BT protocols to save memory
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE)); // ble only
    LOG_DEBUG("Bluetooth controller enabled");

    // init Bluedroid library
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    LOG_DEBUG("Bluedroid enabled");

    // register ble event callback
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_cb));

    { // init hid host functionality with callback
        ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

        esp_hidh_config_t hidh_cfg = {
            .callback = hidh_event_cb,
            .event_stack_size = 4096,
            .callback_arg = NULL,
        };
        ESP_ERROR_CHECK(esp_hidh_init(&hidh_cfg));
    }

    ESP_ERROR_CHECK(bhp_ble_setup_security());

    return ESP_OK;
}

// clang-format off
#define ESP_BLE_SET_SECURITY_PARAM(kind, value) do { \
    ESP_ERROR_CHECK(                                 \
        esp_ble_gap_set_security_param(              \
            (kind),                                  \
            &value,                                  \
            sizeof(value)                            \
        )                                            \
    );                                               \
} while (0)
// clang-format on

esp_err_t bhp_ble_setup_security(void)
{
    // host supports MITM protection + bonding
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT; // host have a screen (= uart console)

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    // use static passkey for pairing BLE devices,
    // so it is easy to connect without seeing output logs
    static uint32_t PAIRING_PASSKEY = 123456;

    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_AUTHEN_REQ_MODE, auth_req);
    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_IOCAP_MODE, iocap);
    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_SET_INIT_KEY, init_key);
    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_SET_RSP_KEY, rsp_key);
    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_MAX_KEY_SIZE, key_size);
    ESP_BLE_SET_SECURITY_PARAM(ESP_BLE_SM_SET_STATIC_PASSKEY, PAIRING_PASSKEY);

    esp_ble_gap_config_local_privacy(true);
    return ESP_OK;
}

bool bhp_ble_start_scan(uint32_t timeout_sec, uint16_t target_appearance)
{
    if (s_is_scanning) {
        return false;
    }
    s_is_scanning = true;

    s_target_appearance = target_appearance;
    s_target_found = false;
    s_scan_timeout_sec = timeout_sec;

    static esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };

    esp_ble_gap_set_scan_params(&scan_params);

    LOG_DEBUG(
        "Starting BLE scan (%d seconds). Looking for appearance = 0x%04X",
        timeout_sec,
        target_appearance
    );

    // Check for errors just in case!
    esp_err_t ret = esp_ble_gap_start_scanning(timeout_sec);
    if (ret != ESP_OK) {
        LOG_ERROR("Failed to start scan! Error: 0x%X", ret);
        s_is_scanning = false;
        return false;
    }

    return true;
}
