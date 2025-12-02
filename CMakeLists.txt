cmake_minimum_required(VERSION 3.20)

# Board configuration - nRF52840 Dongle or DK
# Uncomment the one you're using:
set(BOARD nrf52840dk_nrf52840)
# set(BOARD nrf52840dongle_nrf52840)

set(CONF_FILE "prj.conf")
set(DTC_OVERLAY_FILE "app.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(brewer-ble-hid-bridge VERSION 1.0.0)

target_sources(app PRIVATE
    src/main.c
    src/ble_hid_service.c
    src/hid_keyboard.c
    src/hid_mouse.c
    src/hid_consumer.c
    src/protocol.c
    src/security.c
    src/config.c
    src/gpio_control.c
)

zephyr_library_include_directories(inc)
