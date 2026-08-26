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

add_executable(usb2tft
    main.c
    usb_descriptors.c
    audio.c
)

# I2S output for the MAX98357A.  The RP2040 has no I2S peripheral, so this is a
# PIO program and the header it generates is what audio.c includes.
pico_generate_pio_header(usb2tft ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)

# Continuous 440 Hz tone instead of USB audio, for proving the amp and the
# wiring on their own:  cmake -B build-tone -DAUDIO_TEST_TONE=ON
option(AUDIO_TEST_TONE "Play a test tone instead of taking audio from USB" OFF)
if (AUDIO_TEST_TONE)
    target_compile_definitions(usb2tft PRIVATE AUDIO_TEST_TONE=1)
endif()

target_include_directories(usb2tft PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usb2tft
    pico_stdlib
    pico_bootrom
    hardware_spi
    hardware_dma
    hardware_pio
    tinyusb_device
    tinyusb_board
)

pico_add_extra_outputs(usb2tft)
