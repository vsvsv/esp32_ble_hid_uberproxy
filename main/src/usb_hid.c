#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "common.h"
#include "usb_hid.h"

// clang-format off
// (1) is keyboard and (2) is consumer control (for f-row media buttons to work)
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(2))
};
// clang-format on

static const char* hid_string_descriptor[] = {
    (char[]){ 0x09, 0x04 }, // descriptor language
    "BLE HID UberProxy", // manufacturer
    "Bluetooth Keyboard", // device name
    "fake_serial_69_420", // serial number
    "HID Interface", // type
    NULL,
};

// how frequently usb bus poll the data
// we want the fastest possible to minimize input lag
#define USB_POLLING_FREQUENCY_MS 1

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)
static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(
        0,
        4,
        false,
        sizeof(hid_report_descriptor),
        0x81,
        16,
        USB_POLLING_FREQUENCY_MS
    ),
};

// --- tinyusb callbacks ---

// host asks for a descriptor
const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance)
{ return hid_report_descriptor; }

// host asks for a report (of some sort)
uint16_t tud_hid_get_report_cb(
    uint8_t inst,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen
)
{ return 0; }

// host sends data to the esp
void tud_hid_set_report_cb(
    uint8_t inst,
    uint8_t report_id,
    hid_report_type_t report_type,
    const uint8_t* buffer,
    uint16_t bufsize
)
{
    // do nothing for now
}

// --- usb communication procedures ---

esp_err_t bhp_usb_initialize_as_keyboard(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = NULL;
    cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    cfg.descriptor.string = hid_string_descriptor;
    cfg.descriptor.string_count =
        sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]) - 1;

    return tinyusb_driver_install(&cfg);
}

// checks that the device was connected to a host (PC)
bool bhp_usb_hid_ready(void)
{ return tud_mounted() && tud_hid_ready(); }
