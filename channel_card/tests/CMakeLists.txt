cmake_minimum_required(VERSION 3.22)
project(channel_usb_tests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
                    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
                    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
enable_testing()
add_executable(channel_usb_test test_usb.c ../app/usb.cpp ../app/stream.cpp ../app/samples.cpp)
target_include_directories(channel_usb_test PRIVATE stubs ../app ../berry_runtime)
target_compile_definitions(channel_usb_test PRIVATE USB_RAM=)
target_compile_options(channel_usb_test PRIVATE -Wall -Wextra -Werror)
if(NOT WIN32)
    target_link_libraries(channel_usb_test PRIVATE m)
endif()
add_test(NAME channel_usb COMMAND channel_usb_test)

add_executable(channel_stream_ring_test test_stream_ring.c ../app/stream.cpp)
target_include_directories(channel_stream_ring_test PRIVATE ../app ../berry_runtime)
target_link_libraries(channel_stream_ring_test PRIVATE m)
target_compile_options(channel_stream_ring_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME channel_stream_ring COMMAND channel_stream_ring_test)

add_subdirectory(../berry_runtime script_runtime)
add_executable(channel_stream_playback_test test_stream_playback.c
    ../app/voice.cpp ../app/stream.cpp ../app/samples.cpp
    ../app/filter.cpp ../app/oscillator.cpp)
target_include_directories(channel_stream_playback_test PRIVATE ../app)
target_compile_options(channel_stream_playback_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(channel_stream_playback_test PRIVATE script_runtime::berry berry_runtime m)
add_test(NAME channel_stream_playback COMMAND channel_stream_playback_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_handover.bec")
