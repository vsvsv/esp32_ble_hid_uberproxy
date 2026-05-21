/*
 * BLE-to-USB HID Proxy — ESP32-S3
 *
 * Connects to a BLE HID keyboard (Microsoft Designer Keyboard) as a
 * BLE Central / GATT Client, then forwards the 8-byte keyboard HID
 * reports to the host PC over the native USB OTG port using TinyUSB.
 *
 * BLE stack:   Bluedroid (BLE-only, no Classic BT on ESP32-S3)
 * HID Host:    esp_hidh component (service discovery + report notifications)
 * USB Device:  esp_tinyusb managed component (HID keyboard device class)
 * Pairing:     Passkey Entry (ESP_IO_CAP_OUT) — passkey shown on UART
 * Bonding:     NVS-backed — reconnect without re-pairing after reboot
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ESP system */
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"

/* Bluetooth */
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"

/* TinyUSB */
#include "class/hid/hid_device.h"
#include "tinyusb.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

/* HID Host (esp_hid component) */
#include "esp_hid_common.h"
#include "esp_hidh.h"

static const char* TAG = "BLE_HID_PROXY";

/* ─────────────────────────────────────────────────────────────────────
 * Section 1 — USB HID Descriptors
 * ───────────────────────────────────────────────────────────────────── */

/*
 * Report descriptor: keyboard only, using Report ID 1.
 * Boot-protocol compatible 8-byte report:
 *   [modifier, reserved, key0 .. key5]
 */
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
};

static const char* __attribute__((unused)) hid_string_descriptor[] = {
    (char[]){ 0x09, 0x04 }, /* 0: Supported language = English (0x0409) */
    "BLE HID Proxy", /* 1: Manufacturer */
    "BLE-to-USB Keyboard", /* 2: Product */
    "123456", /* 3: Serial */
    "HID Keyboard"  /* 4: HID Interface */
};

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static const uint8_t __attribute__((unused)) hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/* ─────────────────────────────────────────────────────────────────────
 * Section 2 — TinyUSB HID Callbacks
 * ───────────────────────────────────────────────────────────────────── */

const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen
)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    const uint8_t* buffer,
    uint16_t bufsize
)
{
    /* LED status from host — could forward to BLE keyboard, ignored for now */
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/* ─────────────────────────────────────────────────────────────────────
 * Section 3 — BLE GAP Security Configuration & Event Handler
 * ───────────────────────────────────────────────────────────────────── */

/*
 * Passkey generated once (random 6-digit), stored as static so that
 * the same passkey is used if the stack asks again before bonding
 * completes.  After bonding, NVS stores the keys and passkey is
 * never needed again.
 */
static uint32_t s_passkey = 123456;

static void generate_passkey(void)
{
    /* Hardcoded to 123456 because UART is unavailable */
    ESP_LOGI(TAG, "Passkey hardcoded to: %06" PRIu32, s_passkey);
}

static esp_err_t init_ble_security(void)
{
    esp_err_t ret;

    /* Require Secure Connections + MITM protection + Bonding */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;

    /* ESP32 can only display — keyboard will input the passkey */
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;

    /* Key distribution: exchange encryption + identity keys */
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    /* Generate random passkey and set as static */
    generate_passkey();

    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    if (ret) {
        ESP_LOGE(TAG, "set AUTHEN_REQ failed: %d", ret);
        return ret;
    }

    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    if (ret) {
        ESP_LOGE(TAG, "set IOCAP failed: %d", ret);
        return ret;
    }

    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    if (ret) {
        ESP_LOGE(TAG, "set INIT_KEY failed: %d", ret);
        return ret;
    }

    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    if (ret) {
        ESP_LOGE(TAG, "set RSP_KEY failed: %d", ret);
        return ret;
    }

    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    if (ret) {
        ESP_LOGE(TAG, "set MAX_KEY_SIZE failed: %d", ret);
        return ret;
    }

    ret =
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &s_passkey, sizeof(uint32_t));
    if (ret) {
        ESP_LOGE(TAG, "set STATIC_PASSKEY failed: %d", ret);
        return ret;
    }

    /* Enable Local Privacy to resolve RPA (Resolvable Private Addresses) */
    esp_ble_gap_config_local_privacy(true);

    return ESP_OK;
}

static const char* ble_key_type_str(esp_ble_key_type_t key_type)
{
    switch (key_type) {
        case ESP_LE_KEY_NONE:
            return "NONE";
        case ESP_LE_KEY_PENC:
            return "PENC";
        case ESP_LE_KEY_PID:
            return "PID";
        case ESP_LE_KEY_PCSRK:
            return "PCSRK";
        case ESP_LE_KEY_PLK:
            return "PLK";
        case ESP_LE_KEY_LLK:
            return "LLK";
        case ESP_LE_KEY_LENC:
            return "LENC";
        case ESP_LE_KEY_LID:
            return "LID";
        case ESP_LE_KEY_LCSRK:
            return "LCSRK";
        default:
            return "UNKNOWN";
    }
}

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    switch (event) {
        /* ── Passkey display (ESP_IO_CAP_OUT) ── */
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
            ESP_LOGI(
                TAG,
                "║  PASSKEY: %06" PRIu32 "                    ║",
                param->ble_security.key_notif.passkey
            );
            ESP_LOGI(TAG, "║  Type this on the BLE keyboard       ║");
            ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            break;

        /* ── Peer requests security — auto-accept ── */
        case ESP_GAP_BLE_SEC_REQ_EVT:
            ESP_LOGI(TAG, "BLE SEC_REQ — accepting");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        /* ── Key exchange progress ── */
        case ESP_GAP_BLE_KEY_EVT:
            ESP_LOGD(
                TAG, "BLE KEY type=%s", ble_key_type_str(param->ble_security.ble_key.key_type)
            );
            break;

        /* ── Authentication complete ── */
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (param->ble_security.auth_cmpl.success) {
                ESP_LOGI(
                    TAG,
                    "BLE AUTH SUCCESS — bonded, addr_type=%d",
                    param->ble_security.auth_cmpl.addr_type
                );
            } else {
                ESP_LOGE(
                    TAG, "BLE AUTH FAILED reason=0x%x", param->ble_security.auth_cmpl.fail_reason
                );
            }
            break;

        /* ── Numeric comparison (shouldn't fire with IO_CAP_OUT, handle anyway) ── */
        case ESP_GAP_BLE_NC_REQ_EVT:
            ESP_LOGI(
                TAG,
                "BLE NC_REQ passkey:%" PRIu32 " — confirming",
                param->ble_security.key_notif.passkey
            );
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
            break;

        /* ── Passkey input request (shouldn't fire with IO_CAP_OUT) ── */
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            ESP_LOGW(TAG, "BLE PASSKEY_REQ — replying with generated passkey");
            esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, s_passkey);
            break;

        default:
            ESP_LOGV(TAG, "BLE GAP event %d", event);
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Section 4 — BLE Scan helpers
 * ───────────────────────────────────────────────────────────────────── */

/* Semaphore signalled when BLE scan completes */
static SemaphoreHandle_t s_scan_done_sem = NULL;

/* Scan results storage */
typedef struct scan_result {
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    uint16_t appearance;
    char name[64];
    int8_t rssi;
    struct scan_result* next;
} scan_result_t;

static scan_result_t* s_scan_results = NULL;

static void free_scan_results(void)
{
    scan_result_t* r = s_scan_results;
    while (r) {
        scan_result_t* next = r->next;
        free(r);
        r = next;
    }
    s_scan_results = NULL;
}

static void ble_scan_result_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        return;
    }

    struct ble_scan_result_evt_param* sr = &param->scan_rst;

    switch (sr->search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            /* Check for HID service UUID (0x1812) in advertisement */
            uint8_t uuid_len = 0;
            uint8_t* uuid_d = esp_ble_resolve_adv_data_by_type(
                sr->ble_adv,
                sr->adv_data_len + sr->scan_rsp_len,
                ESP_BLE_AD_TYPE_16SRV_CMPL,
                &uuid_len
            );
            if (!uuid_d || uuid_len < 2) {
                uuid_d = esp_ble_resolve_adv_data_by_type(
                    sr->ble_adv,
                    sr->adv_data_len + sr->scan_rsp_len,
                    ESP_BLE_AD_TYPE_16SRV_PART,
                    &uuid_len
                );
            }

            bool has_hid_svc = false;
            if (uuid_d && uuid_len >= 2) {
                for (int i = 0; i + 1 < uuid_len; i += 2) {
                    uint16_t uuid16 = uuid_d[i] | (uuid_d[i + 1] << 8);
                    if (uuid16 == ESP_GATT_UUID_HID_SVC) {
                        has_hid_svc = true;
                        break;
                    }
                }
            }
            if (!has_hid_svc) {
                break;
            }

            /* Extract appearance */
            uint16_t appearance = 0;
            uint8_t app_len = 0;
            uint8_t* app_d = esp_ble_resolve_adv_data_by_type(
                sr->ble_adv,
                sr->adv_data_len + sr->scan_rsp_len,
                ESP_BLE_AD_TYPE_APPEARANCE,
                &app_len
            );
            if (app_d && app_len >= 2) {
                appearance = app_d[0] | (app_d[1] << 8);
            }

            /* Extract name */
            char name[64] = { 0 };
            uint8_t name_len = 0;
            uint8_t* name_d = esp_ble_resolve_adv_data_by_type(
                sr->ble_adv,
                sr->adv_data_len + sr->scan_rsp_len,
                ESP_BLE_AD_TYPE_NAME_CMPL,
                &name_len
            );
            if (!name_d) {
                name_d = esp_ble_resolve_adv_data_by_type(
                    sr->ble_adv,
                    sr->adv_data_len + sr->scan_rsp_len,
                    ESP_BLE_AD_TYPE_NAME_SHORT,
                    &name_len
                );
            }
            if (name_d && name_len > 0) {
                size_t copy = (name_len < sizeof(name) - 1) ? name_len : sizeof(name) - 1;
                memcpy(name, name_d, copy);
                name[copy] = '\0';
            }

            /* Check if already in results */
            scan_result_t* existing = s_scan_results;
            while (existing) {
                if (memcmp(existing->bda, sr->bda, sizeof(esp_bd_addr_t)) == 0) {
                    break;
                }
                existing = existing->next;
            }
            if (existing) {
                break;
            }

            /* Add to list */
            scan_result_t* r = calloc(1, sizeof(scan_result_t));
            if (!r) {
                break;
            }
            memcpy(r->bda, sr->bda, sizeof(esp_bd_addr_t));
            r->addr_type = sr->ble_addr_type;
            r->appearance = appearance;
            r->rssi = sr->rssi;
            strncpy(r->name, name, sizeof(r->name) - 1);
            r->next = s_scan_results;
            s_scan_results = r;

            ESP_LOGI(
                TAG,
                "BLE HID found: %02x:%02x:%02x:%02x:%02x:%02x "
                "RSSI=%d APP=0x%04x NAME='%s'",
                sr->bda[0],
                sr->bda[1],
                sr->bda[2],
                sr->bda[3],
                sr->bda[4],
                sr->bda[5],
                sr->rssi,
                appearance,
                name
            );
            break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            ESP_LOGI(TAG, "BLE scan complete");
            if (s_scan_done_sem) {
                xSemaphoreGive(s_scan_done_sem);
            }
            break;
        default:
            break;
    }
}

/* Combined GAP handler dispatches to security + scan handlers */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    ble_gap_event_handler(event, param);
    ble_scan_result_handler(event, param);
}

/* ─────────────────────────────────────────────────────────────────────
 * Section 5 — HID Host (esp_hidh) Callback — Core Forwarding Logic
 * ───────────────────────────────────────────────────────────────────── */

static volatile bool s_is_connected = false;

/* Queue for HID reports to prevent dropping when USB is busy */
static QueueHandle_t s_hid_queue = NULL;

static void hid_scan_task(void* pvParameters);

typedef struct {
    uint8_t modifier;
    uint8_t keycode[6];
} hid_report_t;

static void usb_hid_task(void* arg)
{
    hid_report_t report;
    while (1) {
        if (xQueueReceive(s_hid_queue, &report, portMAX_DELAY) == pdTRUE) {
            /* Wait until USB is ready to send */
            while (!tud_hid_ready()) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            tud_hid_keyboard_report(1, report.modifier, report.keycode);
        }
    }
}

static void
    hidh_event_callback(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t* p = (esp_hidh_event_data_t*)event_data;

    switch (event) {
        case ESP_HIDH_OPEN_EVENT: {
            if (p->open.status == ESP_OK) {
                s_is_connected = true;
                const uint8_t* bda = esp_hidh_dev_bda_get(p->open.dev);
                const char* dev_name = esp_hidh_dev_name_get(p->open.dev);
                ESP_LOGI(
                    TAG,
                    "HID OPEN: %s (%02x:%02x:%02x:%02x:%02x:%02x)",
                    dev_name ? dev_name : "UNKNOWN",
                    bda[0],
                    bda[1],
                    bda[2],
                    bda[3],
                    bda[4],
                    bda[5]
                );
                esp_hidh_dev_dump(p->open.dev, stdout);
            } else {
                ESP_LOGE(TAG, "HID OPEN FAILED status=%d", p->open.status);
            }
            break;
        }

        case ESP_HIDH_BATTERY_EVENT: {
            const uint8_t* bda = esp_hidh_dev_bda_get(p->battery.dev);
            ESP_LOGI(
                TAG,
                "BATTERY: %d%% (%02x:%02x:%02x:%02x:%02x:%02x)",
                p->battery.level,
                bda[0],
                bda[1],
                bda[2],
                bda[3],
                bda[4],
                bda[5]
            );
            break;
        }

        case ESP_HIDH_INPUT_EVENT: {
            /*
             * Core forwarding logic.
             *
             * Standard HID keyboard boot report (8 bytes):
             *   Byte 0:      Modifier keys (bitmask)
             *   Byte 1:      Reserved (0x00)
             *   Bytes 2–7:   Key codes (up to 6 simultaneous keys)
             */
            if (p->input.usage == ESP_HID_USAGE_KEYBOARD && p->input.length >= 8) {
                hid_report_t r;
                r.modifier = p->input.data[0];
                memcpy(r.keycode, &p->input.data[2], 6);

                /* Push to queue so we don't drop it if USB is busy */
                if (s_hid_queue && tud_mounted()) {
                    xQueueSend(s_hid_queue, &r, 0);
                }
            } else if (p->input.usage == ESP_HID_USAGE_KEYBOARD && p->input.length > 0) {
                ESP_LOGW(TAG, "KBD report len=%d (expected 8), dumping:", p->input.length);
                ESP_LOG_BUFFER_HEX(TAG, p->input.data, p->input.length);

                /* Still try to forward if at least modifier + reserved + 1 key */
                if (p->input.length >= 3 && tud_mounted() && tud_hid_ready()) {
                    uint8_t modifier = p->input.data[0];
                    uint8_t keycode[6] = { 0 };
                    size_t key_bytes = (p->input.length - 2 > 6) ? 6 : p->input.length - 2;
                    memcpy(keycode, &p->input.data[2], key_bytes);
                    tud_hid_keyboard_report(1, modifier, keycode);
                }
            } else {
                /* Non-keyboard reports (mouse, consumer control, etc.) — log only */
                ESP_LOGD(
                    TAG,
                    "INPUT usage=%s map=%u id=%u len=%d",
                    esp_hid_usage_str(p->input.usage),
                    p->input.map_index,
                    p->input.report_id,
                    p->input.length
                );
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, p->input.data, p->input.length, ESP_LOG_DEBUG);
            }
            break;
        }

        case ESP_HIDH_CLOSE_EVENT: {
            s_is_connected = false;
            const uint8_t* bda = esp_hidh_dev_bda_get(p->close.dev);
            ESP_LOGW(
                TAG,
                "HID CLOSE: %s (%02x:%02x:%02x:%02x:%02x:%02x) — "
                "will rescan in 3s",
                esp_hidh_dev_name_get(p->close.dev),
                bda[0],
                bda[1],
                bda[2],
                bda[3],
                bda[4],
                bda[5]
            );

            /* Send key-release to USB so no keys stuck */
            if (tud_mounted() && tud_hid_ready()) {
                tud_hid_keyboard_report(1, 0, NULL);
            }

            /* Restart scan after short delay */
            xTaskCreate(hid_scan_task, "hid_rescan", 6 * 1024, NULL, 2, NULL);
            break;
        }

        case ESP_HIDH_FEATURE_EVENT:
            ESP_LOGD(TAG, "FEATURE id=%u len=%d", p->feature.report_id, p->feature.length);
            break;

        default:
            ESP_LOGD(TAG, "HIDH event %d", event);
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Section 6 — BLE Scan Task
 * ───────────────────────────────────────────────────────────────────── */

#define SCAN_DURATION_SECONDS 10

static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
};

static void hid_scan_task(void* pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        /* If we are already connected to the keyboard, do nothing! Just sleep. */
        if (s_is_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Looking for bonded devices...");
        int dev_num = esp_ble_get_bond_device_num();
        if (dev_num > 0) {
            esp_ble_bond_dev_t* dev_list = malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
            esp_ble_get_bond_device_list(&dev_num, dev_list);

            /* Try to open bonded devices */
            for (int i = 0; i < dev_num; i++) {
                if (s_is_connected) {
                    break;
                }
                esp_hidh_dev_open(
                    dev_list[i].bd_addr, ESP_HID_TRANSPORT_BLE, dev_list[i].bd_addr_type
                );
            }
            free(dev_list);

            /* Wait up to 5 seconds to see if the connection establishes */
            for (int i = 0; i < 50; i++) {
                if (s_is_connected) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        /* If auto-connect worked, jump back to the top of the loop */
        if (s_is_connected) {
            continue;
        }

        ESP_LOGI(TAG, "Starting active BLE scan...");
        free_scan_results();
        esp_ble_gap_set_scan_params(&scan_params);
        vTaskDelay(pdMS_TO_TICKS(200));

        esp_err_t ret = esp_ble_gap_start_scanning(SCAN_DURATION_SECONDS);
        if (ret == ESP_OK) {
            xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS((SCAN_DURATION_SECONDS + 2) * 1000));
        }

        if (s_is_connected) {
            continue;
        }

        /* Pick best keyboard from scan results */
        scan_result_t* best = NULL;
        scan_result_t* r = s_scan_results;
        while (r) {
            if (!best) {
                best = r;
            } else if (r->appearance == 0x03C1 && best->appearance != 0x03C1) {
                best = r;
            } else if (r->appearance == best->appearance && r->rssi > best->rssi) {
                best = r;
            }
            r = r->next;
        }

        if (best && !s_is_connected) {
            ESP_LOGI(TAG, "Connecting to scanned device: %s", best->name);
            esp_hidh_dev_open(best->bda, ESP_HID_TRANSPORT_BLE, best->addr_type);

            /* Wait up to 5 seconds for connection */
            for (int i = 0; i < 50; i++) {
                if (s_is_connected) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Section 7 — Initialisation (app_main)
 * ───────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t ret;

    /* ── NVS (required for BLE bonding storage) ── */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erasing & re-init");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialised");

    /* ── BLE Controller ── */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_LOGI(TAG, "BT controller enabled (BLE only)");

    /* ── Bluedroid ── */
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGI(TAG, "Bluedroid enabled");

    /* ── GAP callback ── */
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    /* ── GATTC callback (esp_hidh needs this) ── */
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    /* ── BLE Security ── */
    ESP_ERROR_CHECK(init_ble_security());
    ESP_LOGI(TAG, "BLE security configured (IO_CAP_OUT, passkey=%06" PRIu32 ")", s_passkey);

    /* ── esp_hidh init ── */
    esp_hidh_config_t hidh_cfg = {
        .callback = hidh_event_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_cfg));
    ESP_LOGI(TAG, "HID Host initialised");

    /* ── TinyUSB init ── */
    ESP_LOGI(TAG, "USB initialisation ...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialised — HID keyboard device ready");

    /* ── USB HID Queue and Task ── */
    s_hid_queue = xQueueCreate(32, sizeof(hid_report_t));
    xTaskCreate(usb_hid_task, "usb_hid", 4096, NULL, 5, NULL);

    /* ── Scan semaphore ── */
    s_scan_done_sem = xSemaphoreCreateBinary();
    assert(s_scan_done_sem);

    /* ── Log own BLE address ── */
    const uint8_t* own_addr = esp_bt_dev_get_address();
    if (own_addr) {
        ESP_LOGI(
            TAG,
            "Own BLE addr: %02x:%02x:%02x:%02x:%02x:%02x",
            own_addr[0],
            own_addr[1],
            own_addr[2],
            own_addr[3],
            own_addr[4],
            own_addr[5]
        );
    }

    /* ── Launch scan task ── */
    ESP_LOGI(TAG, "Starting BLE HID scan task ...");
    xTaskCreate(hid_scan_task, "hid_scan", 6 * 1024, NULL, 2, NULL);
}
