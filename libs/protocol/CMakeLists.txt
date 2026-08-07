cmake_minimum_required(VERSION 3.16)
project(freshwater_protocol CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pure wire API: command formatters, reply parse, typed clients over IConsoleTransport.
# No serial / RS485 I/O — inject a transport or just use Format* strings.
add_library(protocol STATIC
  src/types.cpp
  src/result.cpp
  src/channel_notes.cpp
  src/channel_mode.cpp
  src/channel_wave.cpp
  src/channel_shape.cpp
  src/channel_envelope.cpp
  src/channel_filter.cpp
  src/channel_gain.cpp
  src/channel_cpu.cpp
  src/effect.cpp
)

add_library(protocol::protocol ALIAS protocol)

target_include_directories(protocol PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MSVC)
  target_compile_options(protocol PRIVATE /W4)
else()
  target_compile_options(protocol PRIVATE -Wall -Wextra)
endif()
