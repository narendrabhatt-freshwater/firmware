cmake_minimum_required(VERSION 3.22)
project(channel_usb_tests LANGUAGES C)
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(channel_usb_test test_usb.c
    ../USB_APP/usb_device.c ../USB_APP/usb_descriptors.c ../USB_APP/usb_app.c
    ../Core/Src/audio/stream_ring.c ../Core/Src/audio/attack_bank.c
    ../Core/Src/console/attack_upload.c)
target_include_directories(channel_usb_test PRIVATE stubs ../USB_APP ../Core/Inc/audio ../Core/Inc/console)
target_compile_definitions(channel_usb_test PRIVATE USB_RAM=)
target_compile_options(channel_usb_test PRIVATE -Wall -Wextra -Werror)
if(NOT WIN32)
    target_link_libraries(channel_usb_test PRIVATE m)
endif()
add_test(NAME channel_usb COMMAND channel_usb_test)

add_executable(channel_stream_ring_test test_stream_ring.c ../Core/Src/audio/stream_ring.c)
target_include_directories(channel_stream_ring_test PRIVATE ../Core/Inc/audio ../USB_APP)
target_compile_options(channel_stream_ring_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME channel_stream_ring COMMAND channel_stream_ring_test)

add_subdirectory(../runtime script_runtime)
add_executable(channel_stream_playback_test test_stream_playback.c
    ../Core/Src/audio/note_bank.c ../Core/Src/audio/stream_ring.c
    ../Core/Src/audio/note_envelope.c ../Core/Src/audio/attack_bank.c
    ../Core/Src/audio/wavetable_osc.c ../Core/Src/audio/channel_vm.c
    ../Core/Src/filters/note_filter.c ../Core/Src/filters/butterworth_four_pole.c)
target_include_directories(channel_stream_playback_test PRIVATE ../Core/Inc/audio
    ../Core/Inc/drivers ../Core/Inc/filters ../USB_APP)
target_compile_options(channel_stream_playback_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(channel_stream_playback_test PRIVATE script_runtime::berry berry_runtime m)
add_test(NAME channel_stream_playback COMMAND channel_stream_playback_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stream_handover.bec")
