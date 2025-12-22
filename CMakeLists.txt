cmake_minimum_required(VERSION 3.20)

# Board can be set via -b flag or defaults to DK
if(NOT DEFINED BOARD)
    set(BOARD nrf52840dk_nrf52840)
endif()

# Base config + board-specific config if it exists
set(CONF_FILE "prj.conf")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}.conf")
    set(CONF_FILE "${CONF_FILE};boards/${BOARD}.conf")
endif()

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
