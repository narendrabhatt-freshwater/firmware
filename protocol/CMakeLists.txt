cmake_minimum_required(VERSION 3.16)
project(cardlink VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
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
set(RTAUDIO_API_JACK OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall" CACHE STRING "" FORCE)
FetchContent_Declare(
  rtaudio
  URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
  URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtaudio)

# RtAudio 6.0.1 installs its config from CMAKE_BINARY_DIR even when embedded.
# Mirror the generated files there so cardlink's install remains usable both
# standalone and as an add_subdirectory dependency.
foreach(RTAUDIO_CONFIG_FILE RtAudioConfig.cmake RtAudioConfig-version.cmake)
  if(EXISTS "${rtaudio_BINARY_DIR}/${RTAUDIO_CONFIG_FILE}")
    configure_file(
      "${rtaudio_BINARY_DIR}/${RTAUDIO_CONFIG_FILE}"
      "${CMAKE_BINARY_DIR}/${RTAUDIO_CONFIG_FILE}"
      COPYONLY
    )
  endif()
endforeach()

if(WIN32)
  set(SERIAL_BACKEND src/serial_port_win32.cpp)
else()
  set(SERIAL_BACKEND src/serial_port_posix.cpp)
endif()

add_library(cardlink STATIC
  ${SERIAL_BACKEND}
  src/cardproto/types.cpp
  src/cardproto/result.cpp
  src/cardproto/channel_notes.cpp
  src/cardproto/channel_shape.cpp
  src/cardproto/channel_envelope.cpp
  src/cardproto/channel_filter.cpp
  src/cardproto/channel_gain.cpp
  src/cardproto/channel_cpu.cpp
  src/cardproto/effect.cpp
  src/rs485/types.cpp
  src/rs485/link.cpp
  src/rs485/transport.cpp
  src/rs485/bus.cpp
  src/rs485/controller.cpp
  src/usb/cdc_transport.cpp
  src/usb/cdc_port.cpp
  src/usb/attack_upload.cpp
  src/usb/vendor_link.cpp
  src/audio/sample_dry.cpp
  src/audio/wave_loader.cpp
  src/audio/audio_engine.cpp
  src/audio/sample_bulk.cpp
  src/midi/pitch.cpp
  src/midi/voice_bank.cpp
  src/midi/midi_input.cpp
  src/sample/client.cpp
)

add_library(cardlink::cardlink ALIAS cardlink)
add_library(cardproto::cardproto ALIAS cardlink)

target_compile_features(cardlink PUBLIC cxx_std_17)

target_include_directories(cardlink PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(cardlink PUBLIC
  $<BUILD_INTERFACE:rtmidi>
  $<BUILD_INTERFACE:rtaudio>
  $<INSTALL_INTERFACE:RtMidi::rtmidi>
  $<INSTALL_INTERFACE:RtAudio::rtaudio>
)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
endif()
if(NOT TARGET PkgConfig::LIBUSB)
  if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
  endif()
  find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
  if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR
      "libusb-1.0 is required for the Channel Card vendor ISO stream "
      "(brew install libusb / apt install libusb-1.0-0-dev)")
  endif()
  add_library(cardlink_libusb INTERFACE)
  target_include_directories(cardlink_libusb INTERFACE ${LIBUSB_INCLUDE_DIR})
  target_link_libraries(cardlink_libusb INTERFACE ${LIBUSB_LIBRARY})
  if(APPLE)
    target_link_libraries(cardlink_libusb INTERFACE "-framework IOKit" "-framework Security")
  endif()
  target_link_libraries(cardlink PUBLIC cardlink_libusb)
else()
  target_link_libraries(cardlink PUBLIC PkgConfig::LIBUSB)
endif()

if(APPLE)
  target_link_libraries(cardlink PUBLIC
    "-framework CoreMIDI"
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioUnit"
  )
endif()

if(MSVC)
  target_compile_options(cardlink PRIVATE /W4)
else()
  target_compile_options(cardlink PRIVATE -Wall -Wextra)
endif()

install(TARGETS cardlink
  EXPORT cardlinkTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cardlinkTargets
  FILE cardlinkTargets.cmake
  NAMESPACE cardlink::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cardlink
)

configure_package_config_file(
  cmake/cardlinkConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cardlinkConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cardlink
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cardlinkConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/cardlinkConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/cardlinkConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cardlink
)

add_executable(cardlink_rs485_rtt tools/rs485_rtt.cpp)
target_link_libraries(cardlink_rs485_rtt PRIVATE cardlink::cardlink)

add_executable(cardlink_sample_hw_smoke tools/sample_hw_smoke.cpp)
target_link_libraries(cardlink_sample_hw_smoke PRIVATE cardlink::cardlink)

include(CTest)
if(BUILD_TESTING)
  add_executable(cardlink_voice_bank_tests tests/voice_bank_test.cpp)
  target_link_libraries(cardlink_voice_bank_tests PRIVATE cardlink::cardlink)
  add_test(NAME cardlink.voice_bank COMMAND cardlink_voice_bank_tests)

  add_executable(cardlink_cardproto_tests tests/cardproto_test.cpp)
  target_link_libraries(cardlink_cardproto_tests PRIVATE cardlink::cardlink)
  add_test(NAME cardlink.cardproto COMMAND cardlink_cardproto_tests)

  add_executable(cardlink_controller_tests tests/controller_test.cpp)
  target_link_libraries(cardlink_controller_tests PRIVATE cardlink::cardlink)
  add_test(NAME cardlink.controller COMMAND cardlink_controller_tests)

  add_executable(cardlink_sample_dry_tests tests/sample_dry_test.cpp)
  target_link_libraries(cardlink_sample_dry_tests PRIVATE cardlink::cardlink)
  add_test(NAME cardlink.sample_dry COMMAND cardlink_sample_dry_tests)

  add_test(
    NAME cardlink.public_api_contract
    COMMAND ${CMAKE_COMMAND}
            -DCARDLINK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_api_contract_test.cmake
  )
endif()
