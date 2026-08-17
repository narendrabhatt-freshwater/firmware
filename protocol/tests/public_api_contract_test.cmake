file(READ "${CARDLINK_SOURCE_DIR}/include/cardproto/channel.hpp" channel_api)

string(REGEX MATCH "[Ww]ave" unsupported_channel_api "${channel_api}")
if(unsupported_channel_api)
  message(FATAL_ERROR
    "The public Channel client must not advertise removed wave commands")
endif()

if(NOT EXISTS "${CARDLINK_SOURCE_DIR}/include/cardlink/usb/cdc_port.hpp")
  message(FATAL_ERROR "The public USB CDC port helper is missing")
endif()

if(NOT EXISTS "${CARDLINK_SOURCE_DIR}/include/cardlink/rs485/controller.hpp")
  message(FATAL_ERROR "The public RS485 controller is missing")
endif()
