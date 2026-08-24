file(READ "${CARDLINK_SOURCE_DIR}/include/cardproto/channel.hpp" channel_api)

string(REGEX MATCH "[Ww]ave" unsupported_channel_api "${channel_api}")
if(unsupported_channel_api)
  message(FATAL_ERROR
    "The public Channel client must not advertise removed wave commands")
endif()

if(NOT EXISTS "${CARDLINK_SOURCE_DIR}/include/cardlink/usb/cdc_port.hpp")
  message(FATAL_ERROR "The public USB CDC port helper is missing")
endif()

file(READ "${CARDLINK_SOURCE_DIR}/include/cardlink/usb/stream_proto.hpp"
  stream_proto_api)
string(FIND "${stream_proto_api}" "kStreamUacSampleBytes = 2"
  uac_int16_api)
if(uac_int16_api EQUAL -1)
  message(FATAL_ERROR "Channel BODY UAC transport must remain 16-bit")
endif()

file(READ "${CARDLINK_SOURCE_DIR}/include/cardlink/audio/sample_dry.hpp"
  sample_dry_api)
string(FIND "${sample_dry_api}" "ApplyVoiceQuery" legacy_vq_api)
if(NOT legacy_vq_api EQUAL -1)
  message(FATAL_ERROR
    "SampleDryMixer must use exact ApplyVoiceStatus credit only")
endif()

if(NOT EXISTS "${CARDLINK_SOURCE_DIR}/include/cardlink/rs485/controller.hpp")
  message(FATAL_ERROR "The public RS485 controller is missing")
endif()
