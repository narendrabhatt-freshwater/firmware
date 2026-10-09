cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_PROJECT_NAME channel_MCU)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C CXX ASM)

add_executable(${CMAKE_PROJECT_NAME})
include(cmake/RS485Config.cmake)
fw_configure_rs485(${CMAKE_PROJECT_NAME} PRIVATE)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
add_subdirectory(berry_runtime ${CMAKE_CURRENT_BINARY_DIR}/script_runtime)

# Keep custom sources here; CubeMX regenerates cmake/stm32cubemx.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    app/cs4304.cpp
    app/audio.cpp
    app/channel.cpp
    app/filter.cpp
    app/oscillator.cpp
    app/voice.cpp
    app/samples.cpp
    app/stream.cpp
    app/usb.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    app
    berry_runtime
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    script_runtime::berry
    berry_runtime
    m
)

# newlib-nano omits floating-point scanf/printf by default. The console parses
# fractional filter/root frequencies and formats floating-point status values.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,-u,_scanf_float
    -Wl,-u,_printf_float
)

# The ELF is an intermediate; app/Makefile delivers the Release binary.
set(FW_INFO_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FirmwareInfo.cmake")
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${FW_INFO_SCRIPT}")
set(FW_INFO_BAUD "${FW_RS485_BAUD}")
if(FW_INFO_BAUD STREQUAL "")
    # Keep direct CMake builds consistent with the fallback used by the firmware.
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/app/channel.h" FW_BAUD_DEFAULT
         REGEX "^#define FW_RS485_BAUD [0-9]+u$")
    string(REGEX REPLACE ".* ([0-9]+)u$" "\\1" FW_INFO_BAUD "${FW_BAUD_DEFAULT}")
endif()
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_COMMAND}
        "-DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        "-DMAP=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
        "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
        "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DBAUD=${FW_INFO_BAUD}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.info"
        -P "${FW_INFO_SCRIPT}"
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
    VERBATIM
)
