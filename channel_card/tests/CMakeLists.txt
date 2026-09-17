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
