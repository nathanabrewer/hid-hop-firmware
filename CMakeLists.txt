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

# Get git hash for build identification
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_HASH)
    set(GIT_HASH "unknown")
endif()

# Build timestamp
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M")

# Pass version info to compiler
target_compile_definitions(app PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_GIT_HASH="${GIT_HASH}"
    APP_BUILD_TIME="${BUILD_TIME}"
)

target_sources(app PRIVATE
    src/main.c
    src/device_mode.c
    src/ble_hid_service.c
    src/hid_keyboard.c
    src/hid_mouse.c
    src/hid_consumer.c
    src/protocol.c
    src/security.c
    src/config.c
    src/gpio_control.c
    src/jsonl_serial.c
    src/mesh_hid.c
)

zephyr_library_include_directories(inc)
