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
set(CMAKE_PROJECT_NAME effect_card)

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
add_subdirectory(../common/vm ${CMAKE_CURRENT_BINARY_DIR}/freshwater_vm)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Hand-written app modules under Core/Src/<domain>/ — CubeMX stays flat.
    Core/Src/console/effect_console.c
    # TinyUSB core + device stack.  Kept in ThirdParty/ (NOT
    # Middlewares/, which CubeMX manages and prunes on regeneration).
    # Version 0.18.0: upgraded from 0.17.0 for its dwc2 isochronous-IN
    # fixes — on 0.17 this core missed ~8.4 IN service intervals per
    # second (measured via 'usbstat': preload ran 991/s against a perfect
    # 1000/s producer), each miss a 1 ms hole the host filled with a
    # discontinuity.  0.18 also splits out dwc2_common.c.
    ThirdParty/tinyusb-0.18/src/tusb.c
    ThirdParty/tinyusb-0.18/src/common/tusb_fifo.c
    ThirdParty/tinyusb-0.18/src/device/usbd.c
    ThirdParty/tinyusb-0.18/src/device/usbd_control.c
    ThirdParty/tinyusb-0.18/src/class/audio/audio_device.c
    ThirdParty/tinyusb-0.18/src/class/cdc/cdc_device.c
    ThirdParty/tinyusb-0.18/src/portable/synopsys/dwc2/dcd_dwc2.c
    ThirdParty/tinyusb-0.18/src/portable/synopsys/dwc2/dwc2_common.c
    # USB application layer (descriptors + UAC2/CDC glue)
    USB_APP/usb_descriptors.c
    USB_APP/usb_app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc/console
    ThirdParty/tinyusb-0.18/src
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
    freshwater::vm

    # Add user defined libraries
)

# Generate .hex and .bin output files from .elf
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.hex and ${CMAKE_PROJECT_NAME}.bin"
)
