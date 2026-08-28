cmake_minimum_required(VERSION 3.20)
project(channel_card_script_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(BERRY_ROOT "${REPO_ROOT}/channel_card/ThirdParty/berry")
file(GLOB BERRY_SOURCES CONFIGURE_DEPENDS "${BERRY_ROOT}/src/*.c")
if(NOT EXISTS "${BERRY_ROOT}/generate/be_const_strtab.h")
    message(FATAL_ERROR "Berry generated tables are missing from the vendored snapshot")
endif()

add_library(script_runtime_core STATIC
    "${REPO_ROOT}/channel_card/Core/Src/script/script_runtime.c")
target_include_directories(script_runtime_core PUBLIC
    "${REPO_ROOT}/channel_card/Core/Inc")
target_compile_options(script_runtime_core PRIVATE -Wall -Wextra -Werror -pedantic)

add_library(berry_host STATIC ${BERRY_SOURCES}
    "${BERRY_ROOT}/default/be_port.c"
    "${BERRY_ROOT}/default/be_modtab.c")
target_include_directories(berry_host PUBLIC
    "${BERRY_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}/config/host")
target_compile_options(berry_host PRIVATE -w)

add_library(berry_runtime STATIC ${BERRY_SOURCES}
    "${REPO_ROOT}/channel_card/Core/Src/script/berry_runtime_modtab.c")
target_include_directories(berry_runtime PUBLIC
    "${BERRY_ROOT}/src" "${REPO_ROOT}/channel_card/Core/Inc/script/berry")
target_compile_options(berry_runtime PRIVATE -w)

add_executable(fw_scriptc fw_scriptc.c)
target_link_libraries(fw_scriptc PRIVATE berry_host script_runtime_core m)
target_compile_options(fw_scriptc PRIVATE -Wall -Wextra -Werror -pedantic)

add_executable(control_runtime_sim control_runtime_sim.c
    "${REPO_ROOT}/channel_card/Core/Src/script/berry_backend.c")
target_link_libraries(control_runtime_sim PRIVATE berry_runtime script_runtime_core m)
target_compile_options(control_runtime_sim PRIVATE -Wall -Wextra -Werror -pedantic)

enable_testing()
add_test(NAME script_runtime_tests COMMAND script_runtime_tests)
add_executable(script_runtime_tests script_runtime_tests.c
    "${REPO_ROOT}/channel_card/Core/Src/script/berry_backend.c")
target_link_libraries(script_runtime_tests PRIVATE berry_runtime script_runtime_core m)
target_compile_options(script_runtime_tests PRIVATE -Wall -Wextra -Werror -pedantic -UNDEBUG)

add_test(NAME compile_baseline
    COMMAND fw_scriptc "${CMAKE_CURRENT_LIST_DIR}/examples/baseline_8x3.be"
            -o "${CMAKE_CURRENT_BINARY_DIR}/baseline_8x3.fwsc")
set_tests_properties(compile_baseline PROPERTIES FIXTURES_SETUP baseline_program)
add_test(NAME simulate_baseline
    COMMAND control_runtime_sim "${CMAKE_CURRENT_BINARY_DIR}/baseline_8x3.fwsc" --ticks 1000)
set_tests_properties(simulate_baseline PROPERTIES FIXTURES_REQUIRED baseline_program
    PASS_REGULAR_EXPRESSION "\"outputs_generated\":24000")

add_test(NAME compile_maximum
    COMMAND fw_scriptc "${CMAKE_CURRENT_LIST_DIR}/examples/maximum_8x10.be"
            -o "${CMAKE_CURRENT_BINARY_DIR}/maximum_8x10.fwsc")
set_tests_properties(compile_maximum PROPERTIES FIXTURES_SETUP maximum_program)
add_test(NAME simulate_maximum
    COMMAND control_runtime_sim "${CMAKE_CURRENT_BINARY_DIR}/maximum_8x10.fwsc" --ticks 1000)
set_tests_properties(simulate_maximum PROPERTIES FIXTURES_REQUIRED maximum_program
    PASS_REGULAR_EXPRESSION "\"outputs_generated\":80000")

add_test(NAME compile_runaway
    COMMAND fw_scriptc "${CMAKE_CURRENT_LIST_DIR}/examples/fault_infinite_loop.be"
            -o "${CMAKE_CURRENT_BINARY_DIR}/fault_infinite_loop.fwsc")
set_tests_properties(compile_runaway PROPERTIES FIXTURES_SETUP runaway_program)
add_test(NAME reject_runaway
    COMMAND control_runtime_sim "${CMAKE_CURRENT_BINARY_DIR}/fault_infinite_loop.fwsc" --ticks 1)
set_tests_properties(reject_runaway PROPERTIES FIXTURES_REQUIRED runaway_program WILL_FAIL TRUE)

add_test(NAME reject_forbidden_builtin
    COMMAND fw_scriptc "${CMAKE_CURRENT_LIST_DIR}/examples/reject_forbidden_builtin.be"
            -o "${CMAKE_CURRENT_BINARY_DIR}/reject_forbidden_builtin.fwsc")
set_tests_properties(reject_forbidden_builtin PROPERTIES WILL_FAIL TRUE)

add_test(NAME verify_runtime_fault_matrix
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=$<TARGET_FILE:fw_scriptc>"
        "-DSIMULATOR=$<TARGET_FILE:control_runtime_sim>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/examples"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/verify_faults.cmake")

add_test(NAME verify_required_program_abi
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=$<TARGET_FILE:fw_scriptc>"
        "-DSIMULATOR=$<TARGET_FILE:control_runtime_sim>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/examples"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/verify_load_failures.cmake")

add_test(NAME verify_million_tick_determinism
    COMMAND "${CMAKE_COMMAND}"
        "-DCOMPILER=$<TARGET_FILE:fw_scriptc>"
        "-DSIMULATOR=$<TARGET_FILE:control_runtime_sim>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/examples"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/verify_long_runs.cmake")
set_tests_properties(verify_million_tick_determinism PROPERTIES LABELS long TIMEOUT 120)

option(SCRIPT_RUNTIME_ARM_FOOTPRINT "Build an ARM object/map footprint probe" OFF)
if(SCRIPT_RUNTIME_ARM_FOOTPRINT)
    add_library(script_runtime_footprint_objects OBJECT
        ${BERRY_SOURCES}
        "${REPO_ROOT}/channel_card/Core/Src/script/berry_runtime_modtab.c"
        "${REPO_ROOT}/channel_card/Core/Src/script/script_runtime.c"
        "${REPO_ROOT}/channel_card/Core/Src/script/berry_backend.c" footprint_probe.c)
    target_include_directories(script_runtime_footprint_objects PRIVATE
        "${BERRY_ROOT}/src" "${REPO_ROOT}/channel_card/Core/Inc/script/berry"
        "${REPO_ROOT}/channel_card/Core/Inc")
    target_compile_options(script_runtime_footprint_objects PRIVATE
        -Os -ffunction-sections -fdata-sections -w)
    add_custom_target(script_runtime_footprint
        COMMAND "${CMAKE_C_COMPILER}" -nostdlib
                "-Wl,-r,-Map=${CMAKE_BINARY_DIR}/script_runtime_footprint.map"
                -o "${CMAKE_BINARY_DIR}/script_runtime_footprint.o"
                $<TARGET_OBJECTS:script_runtime_footprint_objects>
        COMMAND "${CMAKE_SIZE}" -A "${CMAKE_BINARY_DIR}/script_runtime_footprint.o"
        DEPENDS script_runtime_footprint_objects
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
