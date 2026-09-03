cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(cmi_vm_runtime C)
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
    add_subdirectory("${SCRIPT_RUNTIME_ROOT}/../shared/vm"
        "${CMAKE_CURRENT_BINARY_DIR}/freshwater_vm")
endif()

option(SCRIPT_RUNTIME_BUILD_HOST_TOOLS "Build Berry compiler and host qualification" ON)

if(SCRIPT_RUNTIME_BUILD_HOST_TOOLS)
add_library(berry_host OBJECT ${BERRY_SOURCES}
    "${BERRY_ROOT}/default/be_port.c"
    "${SCRIPT_RUNTIME_ROOT}/src/berry_runtime_modtab.c")
target_include_directories(berry_host PUBLIC
    "${BERRY_ROOT}/src" "${SCRIPT_RUNTIME_ROOT}/config/host")
target_compile_options(berry_host PRIVATE -w)
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

if(SCRIPT_RUNTIME_BUILD_HOST_TOOLS)
add_executable(fw_scriptc tools/fw_scriptc.c)
target_include_directories(fw_scriptc PRIVATE "${SCRIPT_RUNTIME_ROOT}/include")
target_link_libraries(fw_scriptc PRIVATE berry_host freshwater::vm m)
target_compile_options(fw_scriptc PRIVATE -Wall -Wextra -Werror -pedantic)

enable_testing()
add_executable(shared_berry_runtime_test tests/shared_runtime_test.c)
target_link_libraries(shared_berry_runtime_test PRIVATE script_runtime::berry)
target_compile_options(shared_berry_runtime_test PRIVATE -Wall -Wextra -Werror -pedantic -UNDEBUG)
add_test(NAME shared_berry.runtime
  COMMAND shared_berry_runtime_test
    "${CMAKE_CURRENT_BINARY_DIR}/channel_envelope.fwsc"
    "${CMAKE_CURRENT_BINARY_DIR}/fault_bad_argument_event.fwsc"
    "${CMAKE_CURRENT_BINARY_DIR}/fault_nonfinite_event.fwsc"
    "${CMAKE_CURRENT_BINARY_DIR}/fault_allocation_event.fwsc"
    "${CMAKE_CURRENT_BINARY_DIR}/fault_runaway_event.fwsc"
    "${CMAKE_CURRENT_BINARY_DIR}/pitch_tracking_event.fwsc")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/channel_envelope.fwsc"
  COMMAND fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/channel_envelope.be"
          -o "${CMAKE_CURRENT_BINARY_DIR}/channel_envelope.fwsc"
  DEPENDS fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/channel_envelope.be")
foreach(FAULT_PROGRAM fault_bad_argument_event fault_nonfinite_event fault_allocation_event fault_runaway_event)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${FAULT_PROGRAM}.fwsc"
    COMMAND fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/${FAULT_PROGRAM}.be"
            -o "${CMAKE_CURRENT_BINARY_DIR}/${FAULT_PROGRAM}.fwsc"
    DEPENDS fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/${FAULT_PROGRAM}.be")
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pitch_tracking_event.fwsc"
  COMMAND fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/pitch_tracking_event.be"
          -o "${CMAKE_CURRENT_BINARY_DIR}/pitch_tracking_event.fwsc"
  DEPENDS fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/pitch_tracking_event.be")
add_custom_target(shared_berry_program DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/channel_envelope.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/fault_bad_argument_event.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/fault_nonfinite_event.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/fault_allocation_event.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/fault_runaway_event.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/pitch_tracking_event.fwsc")
add_dependencies(shared_berry_runtime_test shared_berry_program)

add_executable(led_berry_runtime_test tests/led_runtime_test.c)
target_link_libraries(led_berry_runtime_test PRIVATE script_runtime::berry)
target_compile_options(led_berry_runtime_test PRIVATE
    -Wall -Wextra -Werror -pedantic -UNDEBUG)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/led_event.fwsc"
  COMMAND fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/led_event.be"
          -o "${CMAKE_CURRENT_BINARY_DIR}/led_event.fwsc"
  DEPENDS fw_scriptc "${SCRIPT_RUNTIME_ROOT}/examples/led_event.be")
add_custom_target(led_berry_program DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/led_event.fwsc")
add_dependencies(led_berry_runtime_test led_berry_program)
add_test(NAME shared_berry.led
  COMMAND led_berry_runtime_test
    "${CMAKE_CURRENT_BINARY_DIR}/led_event.fwsc")
endif()
