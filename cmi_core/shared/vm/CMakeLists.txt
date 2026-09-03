cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(freshwater_vm LANGUAGES C)
  include(CTest)
endif()

add_library(freshwater_vm STATIC src/vm.c src/vm_source.c)
add_library(freshwater::vm ALIAS freshwater_vm)
set_target_properties(freshwater_vm PROPERTIES EXPORT_NAME vm)

target_include_directories(freshwater_vm PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(freshwater_vm PUBLIC c_std_11)
target_link_libraries(freshwater_vm PUBLIC m)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(freshwater_vm PRIVATE -Wall -Wextra -Werror)
endif()
