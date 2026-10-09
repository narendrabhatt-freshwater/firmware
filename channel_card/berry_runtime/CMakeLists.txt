cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(channel_vm_runtime C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(BERRY_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/berry")
if(NOT EXISTS "${BERRY_ROOT}/generate/be_const_strtab.h")
    message(FATAL_ERROR "Berry generated tables are missing from the vendored snapshot")
endif()

if(NOT TARGET berry_vm)
    add_library(berry_vm STATIC vm.c vm_source.c)
    add_library(berry::vm ALIAS berry_vm)
    set_target_properties(berry_vm PROPERTIES EXPORT_NAME vm)
    target_include_directories(berry_vm PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_features(berry_vm PUBLIC c_std_11)
    target_link_libraries(berry_vm PUBLIC m)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(berry_vm PRIVATE -Wall -Wextra -Werror)
    endif()
endif()

file(GLOB BERRY_SOURCES CONFIGURE_DEPENDS "${BERRY_ROOT}/src/*.c")
add_library(berry_runtime STATIC ${BERRY_SOURCES} berry_runtime_modtab.c)
target_include_directories(berry_runtime PUBLIC
    "${BERRY_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(berry_runtime PRIVATE -w -ffunction-sections -fdata-sections)

add_library(script_runtime_berry_backend STATIC berry_backend.c)
target_include_directories(script_runtime_berry_backend PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}" "${BERRY_ROOT}/src")
target_link_libraries(script_runtime_berry_backend
    PUBLIC berry::vm PRIVATE berry_runtime m)
target_compile_options(script_runtime_berry_backend PRIVATE
    -Wall -Wextra -Werror -pedantic)
add_library(script_runtime::berry ALIAS script_runtime_berry_backend)
