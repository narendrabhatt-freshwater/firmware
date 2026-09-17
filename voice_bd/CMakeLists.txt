cmake_minimum_required(VERSION 3.24)
project(voicebd LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
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
    PkgConfig::SERIAL PkgConfig::MIDI Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(voicebd PRIVATE PkgConfig::ALSA)
endif()
add_dependencies(voicebd voicebd_program)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/series2.bec"
    COMMAND "${BERRY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/series2.be"
        -o "${CMAKE_CURRENT_BINARY_DIR}/series2.bec"
    DEPENDS "${BERRY_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/series2.be"
    VERBATIM)
add_custom_target(voicebd_gui_program
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/series2.bec" "${CMAKE_CURRENT_SOURCE_DIR}/series2.bec"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/series2.bec")
add_executable(voicebdgui voicebdgui.cpp voicebd.cpp)
target_compile_features(voicebdgui PRIVATE cxx_std_17)
target_compile_options(voicebdgui PRIVATE -Wall -Wextra -Werror)
target_link_libraries(voicebdgui PRIVATE PkgConfig::SERIAL PkgConfig::MIDI Threads::Threads)
set_target_properties(voicebdgui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(voicebdgui voicebd_gui_program)

include(CTest)
if(BUILD_TESTING)
    add_executable(voicebd_gui_test voicebdgui_test.cpp voicebd.cpp)
    target_compile_features(voicebd_gui_test PRIVATE cxx_std_17)
    target_compile_options(voicebd_gui_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(voicebd_gui_test PRIVATE PkgConfig::SERIAL PkgConfig::MIDI Threads::Threads)
    add_test(NAME voicebd_gui COMMAND voicebd_gui_test)
    add_executable(voicebd_hardware_test EXCLUDE_FROM_ALL voicebdtest.cpp)
    target_compile_features(voicebd_hardware_test PRIVATE cxx_std_17)
    target_compile_definitions(voicebd_hardware_test PRIVATE VOICEBD_HARDWARE_TEST=1)
    target_compile_options(voicebd_hardware_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(voicebd_hardware_test PRIVATE PkgConfig::SERIAL Threads::Threads)
endif()
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../channel_card/Core/Src/audio/stream_ring.c")
    add_executable(voicebd_transport_test voicebdtest.cpp
        ../channel_card/Core/Src/audio/stream_ring.c)
    target_compile_features(voicebd_transport_test PRIVATE cxx_std_17)
    target_compile_definitions(voicebd_transport_test PRIVATE VOICEBD_TRANSPORT_TEST=1)
    target_compile_options(voicebd_transport_test PRIVATE -Wall -Wextra -Werror)
    target_include_directories(voicebd_transport_test PRIVATE
        ../channel_card/Core/Inc/audio ../channel_card/USB_APP)
    target_link_libraries(voicebd_transport_test PRIVATE PkgConfig::SERIAL Threads::Threads)
    add_test(NAME voicebd_transport COMMAND voicebd_transport_test)
endif()
