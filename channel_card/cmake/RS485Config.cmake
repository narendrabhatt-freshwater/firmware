include_guard(GLOBAL)

# Empty means use the Channel Card default in console/rs485_config.h.
set(FW_RS485_BAUD "" CACHE STRING "RS485 baud override (empty uses the shared default)")
if(NOT FW_RS485_BAUD STREQUAL "")
  if(NOT FW_RS485_BAUD MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "FW_RS485_BAUD must be a positive integer baud rate")
  endif()
  if(FW_RS485_BAUD GREATER 4000000)
    message(FATAL_ERROR "FW_RS485_BAUD must not exceed 4000000")
  endif()
endif()

function(fw_configure_rs485 target visibility)
  if(NOT FW_RS485_BAUD STREQUAL "")
    target_compile_definitions(${target} ${visibility} FW_RS485_BAUD=${FW_RS485_BAUD}u)
  endif()
endfunction()
