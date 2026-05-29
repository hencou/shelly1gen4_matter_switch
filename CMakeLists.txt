cmake_minimum_required(VERSION 3.5)

# Vereist: export ESP_MATTER_PATH naar je esp-matter checkout
# (zie INSTALL_VSCODE_WINDOWS.md).
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif()

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(shelly1gen4_matter_switch)

# CHIP_HAVE_CONFIG_H is cruciaal — anders mist platform target selectie
# (ChipDevicePlatformEvent typedef, ConnectivityManagerImpl static_cast).
# Volgt het pattern van esp-matter's eigen voorbeelden (examples/light_switch).
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
