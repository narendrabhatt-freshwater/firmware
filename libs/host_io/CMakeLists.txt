cmake_minimum_required(VERSION 3.16)
project(host_io CXX)

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

# Freshwater PC host I/O: shared serial + RS485 tagged link + USB CDC.
add_library(host_io STATIC
  ${SERIAL_BACKEND}
  src/rs485/types.cpp
  src/rs485/link.cpp
  src/rs485/transport.cpp
  src/rs485/bus.cpp
  src/usb/cdc_transport.cpp
  src/usb/wave_upload.cpp
)

add_library(host_io::host_io ALIAS host_io)

target_include_directories(host_io PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(host_io PUBLIC protocol::protocol)

if(MSVC)
  target_compile_options(host_io PRIVATE /W4)
else()
  target_compile_options(host_io PRIVATE -Wall -Wextra)
endif()
