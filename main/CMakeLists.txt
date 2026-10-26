# The firmware. Arduino-ESP32, esp32-camera, esp_jpeg and TFLite Micro come
# from main/idf_component.yml (the component manager adds them here).
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES DFRobot_AXP313A VQF ov5640_af
             app_update bootloader_support efuse esp_http_server esp_hw_support
             esp_netif esp_partition esp_psram esp_wifi lwip mbedtls nvs_flash
             # Arduino's WiFiGeneric.h includes its header (WiFiProv itself
             # isn't compiled)
             espressif__network_provisioning)

idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(python PYTHON)
set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# web/index.html → generated/web_index.h (gzipped, served from flash)
add_custom_command(
    OUTPUT ${gen}/web_index.h
    COMMAND ${python} ${project_dir}/scripts/embed_web.py
            ${project_dir}/web/index.html ${gen}/web_index.h
    DEPENDS ${project_dir}/web/index.html ${project_dir}/scripts/embed_web.py
    VERBATIM)
add_custom_target(web_index DEPENDS ${gen}/web_index.h)

# generated/version.h (git describe or $FW_VERSION, rewritten only when it
# changes) and generated/build_time.h (every build) — runs every build, since
# the git state isn't a file CMake can depend on
add_custom_target(version_info
    COMMAND ${python} ${project_dir}/scripts/version.py ${gen}
    BYPRODUCTS ${gen}/version.h ${gen}/build_time.h
    WORKING_DIRECTORY ${project_dir}
    VERBATIM)

add_dependencies(${COMPONENT_LIB} web_index version_info)

# scripts/test_ota_rollback.py: an image that crashes after boot
if(TEST_CRASH_AFTER_BOOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_CRASH_AFTER_BOOT=1)
endif()
