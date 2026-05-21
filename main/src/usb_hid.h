#ifndef BHP_USB_HID
#define BHP_USB_HID

#include "esp_err.h"
#include "class/hid/hid_device.h"

// --- tinyusb callbacks ---

// host asks for a descriptor
const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance);

// host asks for a report (of some sort)
uint16_t tud_hid_get_report_cb(
    uint8_t inst,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen
);

// host sends data to the esp
void tud_hid_set_report_cb(
    uint8_t inst,
    uint8_t report_id,
    hid_report_type_t report_type,
    const uint8_t* buffer,
    uint16_t bufsize
);

// --- usb communication procedures ---

// initializes the device as usb keyboard
esp_err_t bhp_usb_initialize_as_keyboard(void);

// checks that the device was connected to a host (PC)
bool bhp_usb_hid_ready(void);

#endif // BHP_USB_HID
