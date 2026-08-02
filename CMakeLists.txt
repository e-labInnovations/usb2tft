cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(usb2tft C CXX ASM)

pico_sdk_init()

add_executable(usb2tft main.c)

target_link_libraries(usb2tft
    pico_stdlib
    hardware_spi
)

pico_add_extra_outputs(usb2tft)
