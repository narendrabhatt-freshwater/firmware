file(READ "${CMI_CORE_SOURCE_DIR}/include/cmi/core.hpp" core_api)

foreach(forbidden_api Exec Cpu I2c Adc Debug Echo Register)
  string(FIND "${core_api}" "${forbidden_api}" forbidden_position)
  if(NOT forbidden_position EQUAL -1)
    message(FATAL_ERROR
      "cmi::Core exposes the internal ${forbidden_api} interface")
  endif()
endforeach()

file(READ "${CMI_CORE_SOURCE_DIR}/CMakeLists.txt" package_cmake)
string(FIND "${package_cmake}"
  "install(FILES include/cmi/core.hpp" public_header_install)
if(public_header_install EQUAL -1)
  message(FATAL_ERROR "The installed cmi::Core header is missing")
endif()

foreach(required_api "SetupReport" "SetupReport discover")
  string(FIND "${core_api}" "${required_api}" required_position)
  if(required_position EQUAL -1)
    message(FATAL_ERROR "The public setup API is missing ${required_api}")
  endif()
endforeach()
