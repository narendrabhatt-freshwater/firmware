cmake_minimum_required(VERSION 3.16)
project(control_gui VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMI_VERSION "${PROJECT_VERSION}")
set(CMI_BUNDLE_ID "com.cmi.control")

include(FetchContent)

# ── GLFW (window + input; Win / macOS / Linux) ─────────────────────────────
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
  URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glfw)

# ── Dear ImGui ─────────────────────────────────────────────────────────────
FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
  URL_HASH SHA256=db3a2e02bfd6c269adf0968950573053d002f40bdfb9ef2e4a90bce804b0f286
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

# ── RtMidi / RtAudio (same pins as midi_host) ──────────────────────────────
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall_gui" CACHE STRING "" FORCE)
FetchContent_Declare(
  rtmidi
  URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
  URL_HASH SHA256=ef7bcda27fee6936b651c29ebe9544c74959d0b1583b716ce80a1c6fea7617f0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtmidi)

set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall_gui" CACHE STRING "" FORCE)
FetchContent_Declare(
  rtaudio
  URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
  URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(rtaudio)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../libs/host_io"
                 "${CMAKE_CURRENT_BINARY_DIR}/host_io")

set(MIDI_HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../midi_host/src")

set(CONTROL_GUI_SOURCES
  src/main.cpp
  src/app.cpp
  src/bus_controller.cpp
  src/console_mirror.cpp
  src/preview_scope.cpp
  src/env_editor.cpp
  src/theme.cpp
  src/widgets.cpp
  src/settings.cpp
  src/toast.cpp
  src/file_dialog.cpp
  src/wave_cdc.cpp
  src/wave_upload_queue.cpp
  src/wave_bank_ui.cpp
  ${MIDI_HOST_SRC}/midi_input.cpp
  ${MIDI_HOST_SRC}/pitch.cpp
  ${MIDI_HOST_SRC}/voice_bank.cpp
  ${MIDI_HOST_SRC}/audio_engine.cpp
)
if(APPLE)
  list(APPEND CONTROL_GUI_SOURCES src/macos_app.mm)
endif()

add_executable(control_gui ${CONTROL_GUI_SOURCES})
if(APPLE)
  set_source_files_properties(src/macos_app.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc")
endif()

# Inter + JetBrains Mono ship under assets/fonts (Stitch design system).
# Roboto from ImGui remains a fallback via FW_FONT_DIR.
target_compile_definitions(control_gui PRIVATE
  FW_UI_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
  FW_FONT_DIR="${imgui_SOURCE_DIR}/misc/fonts"
  CMI_VERSION="${CMI_VERSION}"
  CMI_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

target_include_directories(control_gui PRIVATE
  src
  ${MIDI_HOST_SRC}
)

target_link_libraries(control_gui PRIVATE
  imgui_lib
  rtmidi
  rtaudio
  host_io::host_io
)

# OpenGL — portable across Win / macOS / Linux
find_package(OpenGL REQUIRED)
target_link_libraries(control_gui PRIVATE OpenGL::GL)

if(APPLE)
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  target_link_libraries(control_gui PRIVATE
    ${COREMIDI_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
    ${AUDIOUNIT_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
  )
  # Package as CMI Control.app so the menu bar / Dock / About use CMI, not
  # the raw binary name "control_gui".
  set(CMI_APP_NAME "CMI Control")
  set(CMI_APP_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/${CMI_APP_NAME}.app")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY
  )
  add_custom_command(TARGET control_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMI_APP_BUNDLE}/Contents/MacOS"
            "${CMI_APP_BUNDLE}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "${CMI_APP_BUNDLE}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:control_gui>"
            "${CMI_APP_BUNDLE}/Contents/MacOS/${CMI_APP_NAME}"
    COMMENT "Packaging ${CMI_APP_NAME}.app"
  )
elseif(WIN32)
  target_link_libraries(control_gui PRIVATE winmm)
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(control_gui PRIVATE Threads::Threads)
  # X11 usually pulled by GLFW; ALSA/Pulse via RtAudio
endif()

if(MSVC)
  target_compile_options(control_gui PRIVATE /W4)
else()
  target_compile_options(control_gui PRIVATE -Wall -Wextra)
endif()
