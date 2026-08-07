cmake_minimum_required(VERSION 3.16)
project(rs485 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET protocol)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
    "${CMAKE_CURRENT_BINARY_DIR}/protocol")
endif()

if(WIN32)
  set(SERIAL_BACKEND src/serial_port_win32.cpp)
else()
  set(SERIAL_BACKEND src/serial_port_posix.cpp)
endif()

# Host I/O for Freshwater apps: serial port, tagged RS485 link, CDC, bus helper.
add_library(rs485 STATIC
  src/types.cpp
  src/link.cpp
  ${SERIAL_BACKEND}
  src/rs485_transport.cpp
  src/cdc_transport.cpp
  src/bus.cpp
  src/wave_upload.cpp
)

add_library(rs485::rs485 ALIAS rs485)

target_include_directories(rs485 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(rs485 PUBLIC protocol::protocol)

if(MSVC)
  target_compile_options(rs485 PRIVATE /W4)
else()
  target_compile_options(rs485 PRIVATE -Wall -Wextra)
endif()
