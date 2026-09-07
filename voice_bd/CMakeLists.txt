cmake_minimum_required(VERSION 3.24)
project(voicebd LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDIO REQUIRED IMPORTED_TARGET rtaudio>=5.2)
pkg_check_modules(SERIAL REQUIRED IMPORTED_TARGET libserialport)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
add_subdirectory(../cmi_core/third_party/rtmidi rtmidi EXCLUDE_FROM_ALL)
add_subdirectory(../cmi_core/runtime runtime EXCLUDE_FROM_ALL)

set(CHANNEL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core/examples/vm/channel/channel.be")
add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec"
    COMMAND berry "${CHANNEL_SOURCE}" -o "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec"
    DEPENDS berry "${CHANNEL_SOURCE}")
add_custom_target(voicebd_program DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/channel.bec")

add_executable(voicebd voicebdtest.cpp voicebd.cpp)
target_compile_features(voicebd PRIVATE cxx_std_17)
target_compile_options(voicebd PRIVATE -Wall -Wextra)
set_target_properties(voicebd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(voicebd PRIVATE PkgConfig::AUDIO PkgConfig::SERIAL rtmidi)
add_dependencies(voicebd voicebd_program)
