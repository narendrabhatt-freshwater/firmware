cmake_minimum_required(VERSION 3.16)
project(rs485_console CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../libs/cardlink"
                 "${CMAKE_CURRENT_BINARY_DIR}/cardlink")

add_executable(rs485_console
  src/main.cpp
)

target_link_libraries(rs485_console PRIVATE cardlink::cardlink)

if(MSVC)
  target_compile_options(rs485_console PRIVATE /W4)
else()
  target_compile_options(rs485_console PRIVATE -Wall -Wextra)
endif()
