cmake_minimum_required(VERSION 3.16)
project(cmi_core VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(SCRIPT_RUNTIME_BUILD_HOST_TOOLS ON CACHE BOOL "" FORCE)
add_subdirectory(runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall" CACHE STRING "" FORCE)
set(rtmidi_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtmidi")
set(rtmidi_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party/rtmidi")
add_subdirectory("${rtmidi_SOURCE_DIR}" "${rtmidi_BINARY_DIR}")

set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_API_JACK OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall" CACHE STRING "" FORCE)
set(rtaudio_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtaudio")
set(rtaudio_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party/rtaudio")
add_subdirectory("${rtaudio_SOURCE_DIR}" "${rtaudio_BINARY_DIR}")

# RtAudio 6.0.1 installs its config from CMAKE_BINARY_DIR even when embedded.
# Mirror the generated files there so this package remains installable both
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

add_library(cmi_core STATIC
  ${SERIAL_BACKEND}
  src/core.cpp
  src/core/controls.cpp
  src/core/playback.cpp
  src/core/runtime.cpp
  src/core/waves.cpp
  src/cardproto/types.cpp
  src/cardproto/result.cpp
  src/cardproto/channel_notes.cpp
  src/cardproto/channel_filter.cpp
  src/cardproto/channel_gain.cpp
  src/cardproto/effect.cpp
  src/rs485/types.cpp
  src/rs485/link.cpp
  src/rs485/transport.cpp
  src/rs485/bus.cpp
  src/rs485/controller.cpp
  src/usb/cdc_transport.cpp
  src/usb/cdc_port.cpp
  src/usb/attack_upload.cpp
  src/vm/compiler.cpp
  src/vm/uploader.cpp
  src/audio/sample_dry.cpp
  src/audio/wave_loader.cpp
  src/audio/audio_engine.cpp
  src/audio/sample_bulk.cpp
  src/midi/pitch.cpp
  src/midi/voice_bank.cpp
  src/midi/midi_input.cpp
  src/sample/client.cpp
)

add_library(cmi::core ALIAS cmi_core)

set_target_properties(cmi_core PROPERTIES
  OUTPUT_NAME cmi_core
  EXPORT_NAME core)

if(MSVC)
  target_compile_options(rtmidi PRIVATE /w)
  target_compile_options(rtaudio PRIVATE /w)
else()
  target_compile_options(rtmidi PRIVATE -w)
  target_compile_options(rtaudio PRIVATE -w)
endif()

target_compile_features(cmi_core PUBLIC cxx_std_17)

target_include_directories(cmi_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/shared/vm/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(cmi_core PUBLIC
  freshwater::vm
  $<BUILD_INTERFACE:rtmidi>
  $<BUILD_INTERFACE:rtaudio>
  $<INSTALL_INTERFACE:RtMidi::rtmidi>
  $<INSTALL_INTERFACE:RtAudio::rtaudio>
)
target_include_directories(cmi_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party/berry/src
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime/config/host)
target_sources(cmi_core PRIVATE $<TARGET_OBJECTS:berry_host>)

if(APPLE)
  target_link_libraries(cmi_core PUBLIC
    "-framework CoreMIDI"
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioUnit"
  )
endif()

if(MSVC)
  target_compile_options(cmi_core PRIVATE /W4)
else()
  target_compile_options(cmi_core PRIVATE -Wall -Wextra)
endif()

install(TARGETS cmi_core
  EXPORT cmiCoreTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(TARGETS freshwater_vm
  EXPORT freshwaterVmTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES include/cmi/core.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cmi)
install(EXPORT cmiCoreTargets
  FILE cmiCoreTargets.cmake
  NAMESPACE cmi::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmi_core
)
install(EXPORT freshwaterVmTargets
  FILE freshwaterVmTargets.cmake
  NAMESPACE freshwater::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmi_core
)

configure_package_config_file(
  cmake/cmi_coreConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/cmi_coreConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmi_core
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cmi_coreConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/cmi_coreConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/cmi_coreConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cmi_core
)
install(FILES README.md SCRIPTING.md
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cmi_core)
install(DIRECTORY examples/vm/channel
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cmi_core/examples/vm)
install(DIRECTORY LICENSES/ DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/cmi_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(cmi_core_api_tests tests/core_api_test.cpp)
  target_link_libraries(cmi_core_api_tests PRIVATE cmi::core)
  add_test(NAME cmi_core.api COMMAND cmi_core_api_tests)

  add_test(
    NAME cmi_core.public_api_contract
    COMMAND ${CMAKE_COMMAND}
            -DCMI_CORE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_api_contract_test.cmake
  )
endif()
