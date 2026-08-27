cmake_minimum_required(VERSION 3.16)
project(channel_card_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

add_executable(note_bank_release_test
  note_bank_release_test.c
  ../Core/Src/audio/note_bank.c
  ../Core/Src/audio/stream_ring.c)
target_include_directories(note_bank_release_test PRIVATE
  ../Core/Inc/audio
  ../Core/Inc/filters
  ../USB_APP)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(note_bank_release_test PRIVATE
    -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME channel_card.stream_ring_handoff
  COMMAND stream_ring_handoff_test)
add_test(NAME channel_card.note_bank_release
  COMMAND note_bank_release_test)
