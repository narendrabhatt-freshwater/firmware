cmake_minimum_required(VERSION 3.24)
project(voicebd LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(AUDIO REQUIRED IMPORTED_TARGET rtaudio>=5.2)
pkg_check_modules(SERIAL REQUIRED IMPORTED_TARGET libserialport)
pkg_check_modules(MIDI REQUIRED IMPORTED_TARGET rtmidi)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(BERRY_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/berry.linux-arm64")
else()
    set(BERRY_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/berry")
endif()
if(NOT EXISTS "${BERRY_EXECUTABLE}" OR IS_DIRECTORY "${BERRY_EXECUTABLE}")
    message(FATAL_ERROR "Place an executable Berry compiler for this machine at ${BERRY_EXECUTABLE}.")
endif()

set(CHANNEL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/channel.be")
add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec"
    COMMAND "${BERRY_EXECUTABLE}" "${CHANNEL_SOURCE}" -o "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec"
    DEPENDS "${BERRY_EXECUTABLE}" "${CHANNEL_SOURCE}"
    VERBATIM)
add_custom_target(voicebd_program DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec")

add_executable(voicebd voicebdtest.cpp voicebd.cpp)
target_compile_features(voicebd PRIVATE cxx_std_17)
target_compile_options(voicebd PRIVATE -Wall -Wextra)
set_target_properties(voicebd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(voicebd PRIVATE
    PkgConfig::AUDIO PkgConfig::SERIAL PkgConfig::MIDI Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(voicebd PRIVATE PkgConfig::ALSA)
endif()
add_dependencies(voicebd voicebd_program)
