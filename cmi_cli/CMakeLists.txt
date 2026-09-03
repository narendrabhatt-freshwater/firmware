cmake_minimum_required(VERSION 3.16)
project(cmi_play LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../cmi_core"
                 "${CMAKE_CURRENT_BINARY_DIR}/cmi_core")

add_executable(cmi_play src/main.cpp)
target_link_libraries(cmi_play PRIVATE cmi::core)

if(MSVC)
  target_compile_options(cmi_play PRIVATE /W4)
else()
  target_compile_options(cmi_play PRIVATE -Wall -Wextra)
endif()
