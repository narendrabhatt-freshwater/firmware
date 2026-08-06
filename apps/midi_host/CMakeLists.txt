cmake_minimum_required(VERSION 3.16)
project(midi_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)

set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall" CACHE STRING "" FORCE)
FetchContent_Declare(
  rtmidi
  URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
  URL_HASH SHA256=ef7bcda27fee6936b651c29ebe9544c74959d0b1583b716ce80a1c6fea7617f0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtmidi)

set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall" CACHE STRING "" FORCE)
FetchContent_Declare(
  rtaudio
  URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
  URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtaudio)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../libs/rs485"
                 "${CMAKE_CURRENT_BINARY_DIR}/rs485")

add_executable(midi_host
  src/main.cpp
  src/midi_input.cpp
  src/pitch.cpp
  src/voice_bank.cpp
  src/auto_player.cpp
  src/audio_engine.cpp
  src/channel_rs485.cpp
)

target_include_directories(midi_host PRIVATE src)

target_link_libraries(midi_host PRIVATE rtmidi rtaudio rs485)

if(APPLE)
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
  target_link_libraries(midi_host PRIVATE
    ${COREMIDI_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
    ${AUDIOUNIT_FRAMEWORK}
  )
endif()

if(MSVC)
  target_compile_options(midi_host PRIVATE /W4)
else()
  target_compile_options(midi_host PRIVATE -Wall -Wextra)
endif()
