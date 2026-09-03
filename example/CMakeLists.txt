cmake_minimum_required(VERSION 3.16)
project(cmi_core_example LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CMI_CORE_EXAMPLE_USE_INSTALLED
       "Build against an installed cmi_core package" OFF)

if(CMI_CORE_EXAMPLE_USE_INSTALLED)
  find_package(cmi_core CONFIG REQUIRED)
else()
  add_subdirectory(../cmi_core ${CMAKE_CURRENT_BINARY_DIR}/cmi_core)
endif()

add_executable(cmi_core_example main.cpp)
target_link_libraries(cmi_core_example PRIVATE cmi::core)

if(MSVC)
  target_compile_options(cmi_core_example PRIVATE /W4)
else()
  target_compile_options(cmi_core_example PRIVATE -Wall -Wextra -Wpedantic)
endif()
