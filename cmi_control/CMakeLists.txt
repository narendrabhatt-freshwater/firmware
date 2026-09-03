cmake_minimum_required(VERSION 3.16)
project(cmi_control VERSION 1.0.0 LANGUAGES C CXX)

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

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core"
                 "${CMAKE_CURRENT_BINARY_DIR}/cmi_core")

set(CMI_CONTROL_SOURCES
  src/main.cpp
  src/app.cpp
  src/bus_controller.cpp
  src/console_mirror.cpp
  src/preview_scope.cpp
  src/card_panels.cpp
  src/theme.cpp
  src/widgets.cpp
  src/settings.cpp
  src/toast.cpp
  src/file_dialog.cpp
  src/sample_ui.cpp
  src/vm_ui.cpp
)
if(APPLE)
  list(APPEND CMI_CONTROL_SOURCES src/macos_app.mm)
endif()

add_executable(cmi_control ${CMI_CONTROL_SOURCES})
if(APPLE)
  set_source_files_properties(src/macos_app.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc")
endif()

# Inter + JetBrains Mono ship under assets/fonts (Stitch design system).
# Roboto from ImGui remains a fallback via FW_FONT_DIR.
target_compile_definitions(cmi_control PRIVATE
  FW_UI_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts"
  FW_FONT_DIR="${imgui_SOURCE_DIR}/misc/fonts"
  CMI_VERSION="${CMI_VERSION}"
  CMI_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  CMI_VM_EXAMPLE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core/examples/vm/channel"
  CMI_VM_EXAMPLE_CACHE_DIR="${CMAKE_CURRENT_BINARY_DIR}/vm_scripts"
)

set(CMI_VM_EXAMPLE_NAMES
  attack_2ms
  gate
  pluck
  velocity_gate
  voice_steal)
set(CMI_VM_EXAMPLE_OUTPUTS)
foreach(CMI_VM_EXAMPLE IN LISTS CMI_VM_EXAMPLE_NAMES)
  set(CMI_VM_EXAMPLE_OUTPUT
      "${CMAKE_CURRENT_BINARY_DIR}/vm_scripts/${CMI_VM_EXAMPLE}.fwsc")
  add_custom_command(
    OUTPUT "${CMI_VM_EXAMPLE_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/vm_scripts"
    COMMAND fw_scriptc
            "${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core/examples/vm/channel/${CMI_VM_EXAMPLE}.be"
            -o "${CMI_VM_EXAMPLE_OUTPUT}"
    DEPENDS fw_scriptc
            "${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core/examples/vm/channel/${CMI_VM_EXAMPLE}.be"
    VERBATIM)
  list(APPEND CMI_VM_EXAMPLE_OUTPUTS "${CMI_VM_EXAMPLE_OUTPUT}")
endforeach()
add_custom_target(cmi_vm_examples DEPENDS ${CMI_VM_EXAMPLE_OUTPUTS})
add_dependencies(cmi_control cmi_vm_examples)

target_include_directories(cmi_control PRIVATE
  src
)

target_link_libraries(cmi_control PRIVATE
  imgui_lib
  cmi::core
)

# OpenGL — portable across Win / macOS / Linux
find_package(OpenGL REQUIRED)
target_link_libraries(cmi_control PRIVATE OpenGL::GL)

if(APPLE)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  target_link_libraries(cmi_control PRIVATE
    ${COCOA_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
  )
  # Package as CMI Control.app so the menu bar / Dock / About use CMI, not
  # the raw binary name "cmi_control".
  set(CMI_APP_NAME "CMI Control")
  set(CMI_APP_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/${CMI_APP_NAME}.app")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY
  )
  add_custom_command(TARGET cmi_control POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMI_APP_BUNDLE}/Contents/MacOS"
            "${CMI_APP_BUNDLE}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "${CMI_APP_BUNDLE}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:cmi_control>"
            "${CMI_APP_BUNDLE}/Contents/MacOS/${CMI_APP_NAME}"
    COMMENT "Packaging ${CMI_APP_NAME}.app"
  )
elseif(WIN32)
  target_link_libraries(cmi_control PRIVATE winmm)
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(cmi_control PRIVATE Threads::Threads)
  # X11 usually pulled by GLFW; ALSA/Pulse via RtAudio
endif()

if(MSVC)
  target_compile_options(cmi_control PRIVATE /W4)
else()
  target_compile_options(cmi_control PRIVATE -Wall -Wextra)
endif()
