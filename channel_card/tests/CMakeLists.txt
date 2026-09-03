cmake_minimum_required(VERSION 3.16)
project(channel_card_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SCRIPT_RUNTIME_BUILD_HOST_TOOLS ON CACHE BOOL "" FORCE)
add_subdirectory(../../cmi_core/runtime ${CMAKE_CURRENT_BINARY_DIR}/script_runtime)

add_executable(stream_ring_handoff_test
  stream_ring_handoff_test.c
  ../Core/Src/audio/stream_ring.c)
target_include_directories(stream_ring_handoff_test PRIVATE
  ../Core/Inc/audio
  ../USB_APP)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(stream_ring_handoff_test PRIVATE
    -Wall -Wextra -Werror)
endif()

add_executable(note_envelope_test
  note_envelope_test.c
  ../Core/Src/audio/note_envelope.c)
target_include_directories(note_envelope_test PRIVATE ../Core/Inc/audio)
target_link_libraries(note_envelope_test PRIVATE m)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(note_envelope_test PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(note_bank_vm_test
  note_bank_vm_test.c
  ../Core/Src/audio/note_bank.c
  ../Core/Src/audio/channel_vm.c
  ../Core/Src/audio/note_envelope.c
  ../Core/Src/audio/stream_ring.c)
target_include_directories(note_bank_vm_test PRIVATE
  ../Core/Inc/audio ../Core/Inc/filters ../Core/Inc/drivers ../USB_APP ../../cmi_core/shared/vm/include
  ../../cmi_core/runtime/include)
target_link_libraries(note_bank_vm_test PRIVATE script_runtime::berry m)
target_compile_definitions(note_bank_vm_test PRIVATE CHANNEL_TEST_WAVETABLE=1)
add_dependencies(note_bank_vm_test shared_berry_program)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/channel.fwsc"
  COMMAND fw_scriptc
          "${CMAKE_CURRENT_SOURCE_DIR}/../../cmi_core/examples/vm/channel/channel.be"
          -o "${CMAKE_CURRENT_BINARY_DIR}/channel.fwsc"
  DEPENDS fw_scriptc
          "${CMAKE_CURRENT_SOURCE_DIR}/../../cmi_core/examples/vm/channel/channel.be")
add_custom_target(channel_program DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/channel.fwsc")
add_dependencies(note_bank_vm_test channel_program)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(note_bank_vm_test PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME channel_card.stream_ring_handoff
  COMMAND stream_ring_handoff_test)
add_test(NAME channel_card.note_envelope COMMAND note_envelope_test)
add_test(NAME channel_card.note_bank_vm COMMAND note_bank_vm_test
  "${CMAKE_CURRENT_BINARY_DIR}/script_runtime/channel_envelope.fwsc"
  "${CMAKE_CURRENT_BINARY_DIR}/channel.fwsc")
