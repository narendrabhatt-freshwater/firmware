cmake_minimum_required(VERSION 3.16)
project(cardlink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET cardproto)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cardproto"
    "${CMAKE_CURRENT_BINARY_DIR}/cardproto")
endif()

if(WIN32)
  set(SERIAL_BACKEND src/serial_port_win32.cpp)
else()
  set(SERIAL_BACKEND src/serial_port_posix.cpp)
endif()

# Freshwater PC host I/O: shared serial + RS485 tagged link + USB CDC.
add_library(cardlink STATIC
  ${SERIAL_BACKEND}
  src/rs485/types.cpp
  src/rs485/link.cpp
  src/rs485/transport.cpp
  src/rs485/bus.cpp
  src/usb/cdc_transport.cpp
  src/usb/wave_upload.cpp
  src/usb/attack_upload.cpp
  src/audio/sample_dry.cpp
  src/audio/wave_loader.cpp
  src/sample/client.cpp
)

add_library(cardlink::cardlink ALIAS cardlink)

target_include_directories(cardlink PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(cardlink PUBLIC cardproto::cardproto)

if(MSVC)
  target_compile_options(cardlink PRIVATE /W4)
else()
  target_compile_options(cardlink PRIVATE -Wall -Wextra)
endif()
