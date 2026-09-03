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
option(CHANNEL_TEST_WAVETABLE
    "Use the small looped test oscillator instead of attack/BODY storage" OFF)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
set(SCRIPT_RUNTIME_BUILD_HOST_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(../cmi_core/runtime ${CMAKE_CURRENT_BINARY_DIR}/script_runtime)

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
    # Hand-written app modules live under Core/Src/<domain>/ — CubeMX
    # sources stay flat in Core/Src. Never add these to cmake/stm32cubemx/.
    Core/Src/drivers/cs4304.c
    Core/Src/drivers/channel_led.c
    Core/Src/audio/note_bank.c
    Core/Src/audio/note_envelope.c
    Core/Src/audio/attack_bank.c
    Core/Src/audio/stream_ring.c
    Core/Src/audio/audio_tone_dc.c
    Core/Src/filters/butterworth_four_pole.c
    Core/Src/filters/note_filter.c
    Core/Src/console/channel_console.c
    Core/Src/console/uart5_rx.c
    Core/Src/console/attack_upload.c
    Core/Src/console/vm_upload.c
    Core/Src/audio/audio_bridge.c
    Core/Src/audio/channel_vm.c
    # TinyUSB core + device stack.  Kept in ThirdParty/ (NOT
    # Middlewares/, which CubeMX manages and prunes on regeneration).
    ThirdParty/tinyusb/src/tusb.c
    ThirdParty/tinyusb/src/common/tusb_fifo.c
    ThirdParty/tinyusb/src/device/usbd.c
    ThirdParty/tinyusb/src/device/usbd_control.c
    ThirdParty/tinyusb/src/class/audio/audio_device.c
    ThirdParty/tinyusb/src/class/cdc/cdc_device.c
    ThirdParty/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    # USB application layer (descriptors + UAC2 BODY + CDC)
    USB_APP/usb_descriptors.c
    USB_APP/usb_app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc/filters
    Core/Inc/audio
    Core/Inc/drivers
    Core/Inc/console
    ThirdParty/tinyusb/src
    USB_APP
    ../cmi_core/shared/vm/include
    ../cmi_core/runtime/include
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    CFG_TUSB_MCU=OPT_MCU_STM32H7
)
if(CHANNEL_TEST_WAVETABLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        CHANNEL_TEST_WAVETABLE=1)
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    script_runtime::berry
    berry_runtime
    m
)

# --specs=nano.specs (cmake/gcc-arm-none-eabi.cmake) strips floating-point
# scanf/printf conversions out of newlib-nano by default to save flash.
# The console parses fractional values with sscanf(..., "%lf", ...)
# (note keys `nX on <key>` and filter cutoffs) and
# formats replies with %.1f/%.2f. Without these, sscanf("%lf") always
# returns 0 -> every such command answers "err:" for valid input.
# Force-link the float variants instead of dropping nano.specs entirely
# (keeps flash usage low everywhere else).
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
