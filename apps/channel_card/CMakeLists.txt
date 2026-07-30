cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME channel_MCU)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
#
# NOTE: hand-written sources belong HERE, not in cmake/stm32cubemx/
# CMakeLists.txt — that file is regenerated from the .ioc and any manual
# additions to it are silently lost (which is exactly how cs4304.c went
# missing after the USB middleware was removed).
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # CS4304 DAC driver (hand-written, not CubeMX-generated)
    Core/Src/cs4304.c
    # N0–NF additive sine bank for CH1 (phase-accumulators + mixer)
    Core/Src/note_bank.c
    # RS485 + USB CDC console, cpuload probe, LED chaser
    Core/Src/channel_console.c
    # USB audio -> I2S bridge (relocated out of USB_DEVICE/ when the card
    # moved from ST's USB Device Library to TinyUSB)
    Core/Src/audio_bridge.c
    # TinyUSB core + device stack.  Kept in ThirdParty/ (NOT
    # Middlewares/, which CubeMX manages and prunes on regeneration).
    ThirdParty/tinyusb/src/tusb.c
    ThirdParty/tinyusb/src/common/tusb_fifo.c
    ThirdParty/tinyusb/src/device/usbd.c
    ThirdParty/tinyusb/src/device/usbd_control.c
    ThirdParty/tinyusb/src/class/audio/audio_device.c
    ThirdParty/tinyusb/src/class/cdc/cdc_device.c
    ThirdParty/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    # USB application layer (descriptors + UAC2/CDC glue)
    USB_APP/usb_descriptors.c
    USB_APP/usb_app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ThirdParty/tinyusb/src
    USB_APP
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    CFG_TUSB_MCU=OPT_MCU_STM32H7
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# --specs=nano.specs (cmake/gcc-arm-none-eabi.cmake) strips floating-point
# scanf/printf conversions out of newlib-nano by default to save flash.
# The console's 'tone1 <Hz>' command relies on sscanf(..., "%lf", ...) to
# support fractional-Hz tones, and its reply (plus 'status') uses %.1f.
# Without these, sscanf("%lf") always returns 0 -> 'tone1' answers
# "err:" for every input, valid or not. Force-link the float variants
# instead of dropping nano.specs entirely (keeps flash usage low
# everywhere else).
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,-u,_scanf_float
    -Wl,-u,_printf_float
)

# Generate .hex and .bin output files from .elf
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.hex and ${CMAKE_PROJECT_NAME}.bin"
)
