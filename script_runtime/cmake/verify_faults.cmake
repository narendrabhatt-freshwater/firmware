if(NOT DEFINED COMPILER OR NOT DEFINED SIMULATOR OR NOT DEFINED SOURCE_DIR OR
   NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "fault verifier arguments are missing")
endif()

set(CASES
    "infinite_loop:5"
    "allocation:6"
    "exception:6"
    "incomplete:9"
    "nan:8"
    "recursion:6"
    "bad_native:10")

foreach(CASE IN LISTS CASES)
    string(REPLACE ":" ";" PARTS "${CASE}")
    list(GET PARTS 0 NAME)
    list(GET PARTS 1 EXPECTED_FAULT)
    set(SOURCE "${SOURCE_DIR}/fault_${NAME}.be")
    set(CONTAINER "${OUTPUT_DIR}/fault_${NAME}.fwsc")
    execute_process(
        COMMAND "${COMPILER}" "${SOURCE}" -o "${CONTAINER}"
        RESULT_VARIABLE COMPILE_RESULT
        OUTPUT_VARIABLE COMPILE_OUT
        ERROR_VARIABLE COMPILE_ERR)
    if(NOT COMPILE_RESULT EQUAL 0)
        message(FATAL_ERROR "failed to compile ${NAME}: ${COMPILE_OUT}${COMPILE_ERR}")
    endif()
    execute_process(
        COMMAND "${SIMULATOR}" "${CONTAINER}" --ticks 1
        RESULT_VARIABLE SIM_RESULT
        OUTPUT_VARIABLE SIM_OUT
        ERROR_VARIABLE SIM_ERR)
    if(SIM_RESULT EQUAL 0)
        message(FATAL_ERROR "fault case ${NAME} unexpectedly succeeded: ${SIM_OUT}")
    endif()
    set(COMBINED "${SIM_OUT}${SIM_ERR}")
    if(NOT COMBINED MATCHES "runtime fault ${EXPECTED_FAULT} at tick 0")
        message(FATAL_ERROR "fault case ${NAME} returned wrong result: ${COMBINED}")
    endif()
endforeach()
