cmake_minimum_required(VERSION 3.16)
project(rs485 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set(SERIAL_BACKEND src/serial_port_win32.cpp)
else()
  set(SERIAL_BACKEND src/serial_port_posix.cpp)
endif()

add_library(rs485 STATIC
  src/types.cpp
  src/link.cpp
  src/session.cpp
  ${SERIAL_BACKEND}
)

add_library(rs485::rs485 ALIAS rs485)

target_include_directories(rs485 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MSVC)
  target_compile_options(rs485 PRIVATE /W4)
else()
  target_compile_options(rs485 PRIVATE -Wall -Wextra)
endif()
