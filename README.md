# BLE HID Uberproxy

Make ESP32-S3 to act as a Bluetooth Low Energy HID host.

It allows to connect BLE HID peripherals (e. g. keyboards, mice) to the ESP32-S3, which then presents them as standard USB HID devices to a connected PC.

Because PC will see them as regular USB wired devices, it allows to use BLE HID devices in environments that do not natively support Bluetooth, such as BIOS or UEFI.


## Building

Before building, install the packages, required by ESP-IDF to your system.

```sh
# For debian/ubuntu
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# For MacOS
brew install cmake ninja dfu-util ccache
```

Then, bootstrap the ESP SDK:

```sh
git submodule update --init --recursive --depth 1

./esp-idf/install.sh esp32s3
source ./esp-idf/export.sh
# source ./esp-idf/export.fish # if you are using fish
```

After bootstrapping the environment, set the target to ESP32-S3 and compile the firmware:

```sh
idf.py set-target esp32s3
idf.py build
```

Flash the firmware to the board:

```sh
idf.py flash

#idf.py monitor  # to see console logs
```

*(Note: Specify serial port if it isn't automatically detected, e.g., `idf.py -p /dev/ttyACM0 flash monitor` or `idf.py -p COM3 flash monitor`.)*
