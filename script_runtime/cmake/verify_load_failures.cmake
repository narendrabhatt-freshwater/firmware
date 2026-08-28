foreach(NAME IN ITEMS missing_tick missing_configuration)
    set(SOURCE "${SOURCE_DIR}/reject_${NAME}.be")
    set(CONTAINER "${OUTPUT_DIR}/reject_${NAME}.fwsc")
    execute_process(
        COMMAND "${COMPILER}" "${SOURCE}" -o "${CONTAINER}"
        RESULT_VARIABLE COMPILE_RESULT
        OUTPUT_VARIABLE COMPILE_OUT ERROR_VARIABLE COMPILE_ERR)
    if(NOT COMPILE_RESULT EQUAL 0)
        message(FATAL_ERROR "failed to compile ${NAME}: ${COMPILE_OUT}${COMPILE_ERR}")
    endif()
    execute_process(
        COMMAND "${SIMULATOR}" "${CONTAINER}" --ticks 1
        RESULT_VARIABLE SIM_RESULT
        OUTPUT_VARIABLE SIM_OUT ERROR_VARIABLE SIM_ERR)
    if(SIM_RESULT EQUAL 0 OR NOT "${SIM_OUT}${SIM_ERR}" MATCHES "upload failed.*fault 11")
        message(FATAL_ERROR "load failure ${NAME} was not rejected safely: ${SIM_OUT}${SIM_ERR}")
    endif()
endforeach()

foreach(CASE IN ITEMS "init_runaway:5" "init_exception:4")
    string(REPLACE ":" ";" PARTS "${CASE}")
    list(GET PARTS 0 NAME)
    list(GET PARTS 1 EXPECTED_FAULT)
    set(SOURCE "${SOURCE_DIR}/reject_${NAME}.be")
    set(CONTAINER "${OUTPUT_DIR}/reject_${NAME}.fwsc")
    execute_process(COMMAND "${COMPILER}" "${SOURCE}" -o "${CONTAINER}"
        RESULT_VARIABLE COMPILE_RESULT OUTPUT_VARIABLE COMPILE_OUT ERROR_VARIABLE COMPILE_ERR)
    if(NOT COMPILE_RESULT EQUAL 0)
        message(FATAL_ERROR "failed to compile ${NAME}: ${COMPILE_OUT}${COMPILE_ERR}")
    endif()
    execute_process(COMMAND "${SIMULATOR}" "${CONTAINER}" --ticks 1
        RESULT_VARIABLE SIM_RESULT OUTPUT_VARIABLE SIM_OUT ERROR_VARIABLE SIM_ERR)
    if(SIM_RESULT EQUAL 0 OR NOT "${SIM_OUT}${SIM_ERR}" MATCHES "upload failed.*fault ${EXPECTED_FAULT}")
        message(FATAL_ERROR "load fault ${NAME} was not rejected safely: ${SIM_OUT}${SIM_ERR}")
    endif()
endforeach()
