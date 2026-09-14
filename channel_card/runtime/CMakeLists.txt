cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(channel_vm_runtime C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(SCRIPT_RUNTIME_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(BERRY_ROOT "${SCRIPT_RUNTIME_ROOT}/third_party/berry")
file(GLOB BERRY_SOURCES CONFIGURE_DEPENDS "${BERRY_ROOT}/src/*.c")
if(NOT EXISTS "${BERRY_ROOT}/generate/be_const_strtab.h")
    message(FATAL_ERROR "Berry generated tables are missing from the vendored snapshot")
endif()

if(NOT TARGET freshwater_vm)
    add_subdirectory("${SCRIPT_RUNTIME_ROOT}/vm"
        "${CMAKE_CURRENT_BINARY_DIR}/freshwater_vm")
endif()

add_library(berry_runtime STATIC ${BERRY_SOURCES}
    "${SCRIPT_RUNTIME_ROOT}/src/berry_runtime_modtab.c")
target_include_directories(berry_runtime PUBLIC
    "${BERRY_ROOT}/src" "${SCRIPT_RUNTIME_ROOT}/include/script/berry")
target_compile_options(berry_runtime PRIVATE -w -ffunction-sections -fdata-sections)

add_library(script_runtime_berry_backend STATIC
    "${SCRIPT_RUNTIME_ROOT}/src/berry_backend.c")
target_include_directories(script_runtime_berry_backend PUBLIC
    "${SCRIPT_RUNTIME_ROOT}/include" "${BERRY_ROOT}/src")
target_link_libraries(script_runtime_berry_backend
    PUBLIC freshwater::vm
    PRIVATE berry_runtime m)
target_compile_options(script_runtime_berry_backend PRIVATE
    -Wall -Wextra -Werror -pedantic)
add_library(script_runtime::berry ALIAS script_runtime_berry_backend)
