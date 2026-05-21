idf_component_register(
    SRCS "src/main.c" "src/usb_hid.c" "src/ble_host.c"
    INCLUDE_DIRS "."
    INCLUDE_DIRS "./src"
    PRIV_REQUIRES nvs_flash esp_hid bt
)
