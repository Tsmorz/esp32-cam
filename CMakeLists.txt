# Wifi-Camera firmware — ESP-IDF project with Arduino-ESP32 as a component.
#   idf.py build                     (after `. $IDF_PATH/export.sh`, IDF v5.5.5)
#   idf.py -DTEST_CRASH_AFTER_BOOT=1 -B build-crash build   (scripts/test_ota_rollback.py)
# Configuration: sdkconfig.defaults (sdkconfig itself is generated, not tracked).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Serial = the ESP32-S3's built-in USB-Serial/JTAG port, as on the FireBeetle
# in the Arduino IDE. Without these Serial maps to UART0 (GPIO 43/44).
list(APPEND compile_definitions "ARDUINO_USB_CDC_ON_BOOT=1")
list(APPEND compile_definitions "ARDUINO_USB_MODE=1")
# The N16R8 has 8 MB PSRAM (lets Arduino's String grow past 64 KB)
list(APPEND compile_definitions "BOARD_HAS_PSRAM")

# The image's app descriptor carries the release version CI is building
# (scripts/next_version.sh); otherwise IDF uses `git describe`
if(NOT "$ENV{FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{FW_VERSION}")
endif()

# Build only main and what it requires, not every downloaded component
idf_build_set_property(MINIMAL_BUILD ON)
project(wifi-camera) # → build/wifi-camera.bin

# esp-modbus (required by Arduino, unused here) builds with -Werror and trips
# GCC 14's stringop-truncation; it's linked out anyway
idf_component_get_property(modbus_lib espressif__esp-modbus COMPONENT_LIB)
target_compile_options(${modbus_lib} PRIVATE -Wno-error=stringop-truncation)
