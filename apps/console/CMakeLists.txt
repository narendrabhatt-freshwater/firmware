cmake_minimum_required(VERSION 3.16)
project(rs485_console CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# One serial backend per OS family — no external serial library. See
# src/serial_port.hpp for the interface both backends implement.
if(WIN32)
  set(SERIAL_BACKEND src/serial_port_win32.cpp)
else()
  set(SERIAL_BACKEND src/serial_port_posix.cpp)
endif()

add_executable(rs485_console
  src/main.cpp
  src/rs485_link.cpp
  ${SERIAL_BACKEND}
)

target_include_directories(rs485_console PRIVATE src)

if(MSVC)
  target_compile_options(rs485_console PRIVATE /W4)
else()
  target_compile_options(rs485_console PRIVATE -Wall -Wextra)
endif()
