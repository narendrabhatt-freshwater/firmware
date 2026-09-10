cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(freshwater_berry_compiler LANGUAGES C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(BERRY_ROOT "${CMAKE_CURRENT_LIST_DIR}/vendor/berry")
if(NOT EXISTS "${BERRY_ROOT}/generate/be_const_strtab.h")
  message(FATAL_ERROR "Berry generated tables are missing from the vendored snapshot")
endif()

if(NOT TARGET freshwater_vm)
  add_library(freshwater_vm STATIC shared/src/vm.c shared/src/vm_source.c)
  add_library(freshwater::vm ALIAS freshwater_vm)
  target_include_directories(freshwater_vm PUBLIC shared/include)
endif()

file(GLOB BERRY_SOURCES CONFIGURE_DEPENDS "${BERRY_ROOT}/src/*.c")
add_library(berry_host OBJECT ${BERRY_SOURCES}
  "${BERRY_ROOT}/default/be_port.c"
  shared/src/berry_runtime_modtab.c)
target_include_directories(berry_host PUBLIC
  "${BERRY_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}/config")
target_compile_options(berry_host PRIVATE -w)

set(BERRY_EXECUTABLE "" CACHE FILEPATH "Use an existing standalone Berry compiler")
if(BERRY_EXECUTABLE)
  if(NOT EXISTS "${BERRY_EXECUTABLE}" OR IS_DIRECTORY "${BERRY_EXECUTABLE}")
    message(FATAL_ERROR "BERRY_EXECUTABLE must name an existing berry executable")
  endif()
  add_executable(berry IMPORTED GLOBAL)
  set_target_properties(berry PROPERTIES IMPORTED_LOCATION "${BERRY_EXECUTABLE}")
else()
  add_executable(berry berry.c)
  target_include_directories(berry PRIVATE shared/include)
  target_link_libraries(berry PRIVATE berry_host freshwater::vm m)
  target_compile_options(berry PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
