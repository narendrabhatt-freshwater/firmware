/* USB descriptors — Channel Card composite device
 *   ITF0/1: UAC2 speaker (8ch int16, 48 kHz) dry voice feeds + feedback EP
 *   ITF2/3: CDC-ACM console
 */

#include "tusb.h"

#define TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL_LEN (6 + (8 + 1) * 4)
#define TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL(                       \
    _unitid, _srcid, _c0, _c1, _c2, _c3, _c4, _c5, _c6, _c7, _c8,       \
    _stridx)                                                             \
  TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL_LEN, TUSB_DESC_CS_INTERFACE, \
      AUDIO_CS_AC_INTERFACE_FEATURE_UNIT, _unitid, _srcid,               \
      U32_TO_U8S_LE(_c0), U32_TO_U8S_LE(_c1), U32_TO_U8S_LE(_c2),        \
      U32_TO_U8S_LE(_c3), U32_TO_U8S_LE(_c4), U32_TO_U8S_LE(_c5),        \
      U32_TO_U8S_LE(_c6), U32_TO_U8S_LE(_c7), U32_TO_U8S_LE(_c8), _stridx

#ifndef TUD_AUDIO_SPEAKER_8CH_FB_DESC_LEN
/* ONE_CHANNEL_LEN is `6+(1+1)*4` without outer parens — must parenthesize
 * the subtraction or CONFIG wTotalLength overshoots and macOS leaves the
 * composite unconfigured (no CDC, no UAC interfaces). */
#define TUD_AUDIO_SPEAKER_8CH_FB_DESC_LEN                                    \
  (TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN -                                      \
   (TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN) +                           \
   TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL_LEN)
#endif

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = 0xCafe, /* placeholder VID/PID; assign production values
                           before release */
    /* Bump PID on any descriptor change (macOS caches by VID/PID).
     * 0x4016 = 8ch SAMPLE dry; 0x4017 = FUNC_1_DESC_LEN fix (CDC enum);
     * 0x4018 = audio interface string "Channel Card" (CoreAudio name). */
    .idProduct = 0x4018,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum {
  ITF_NUM_AUDIO_CONTROL = 0,
  ITF_NUM_AUDIO_STREAMING,
  ITF_NUM_CDC,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN                                                       \
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_SPEAKER_8CH_FB_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_FB 0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83

#define _MUTE_CTRL (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

#define _SPK_ITF ITF_NUM_AUDIO_CONTROL
    TUD_AUDIO_DESC_IAD(_SPK_ITF, 0x02, 0x00),
    TUD_AUDIO_DESC_STD_AC(_SPK_ITF, 0x00, 4),
    TUD_AUDIO_DESC_CS_AC(
        0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,
        TUD_AUDIO_DESC_CLK_SRC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN +
            TUD_AUDIO_DESC_OUTPUT_TERM_LEN +
            TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL_LEN,
        AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),
    TUD_AUDIO_DESC_CLK_SRC(0x04, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                           (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS),
                           0x01, 0x00),
    TUD_AUDIO_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x04,
                              0x08, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,
                              0 * (AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS),
                              0x00),
    TUD_AUDIO_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x01,
                               0x02, 0x04, 0x0000, 0x00),
    TUD_AUDIO_DESC_FEATURE_UNIT_EIGHT_CHANNEL(
        0x02, 0x01, _MUTE_CTRL, _MUTE_CTRL, _MUTE_CTRL, _MUTE_CTRL, _MUTE_CTRL,
        _MUTE_CTRL, _MUTE_CTRL, _MUTE_CTRL, _MUTE_CTRL, 0x00),
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(_SPK_ITF + 1), 0x00, 0x00, 0x00),
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(_SPK_ITF + 1), 0x01, 0x02, 0x00),
    TUD_AUDIO_DESC_CS_AS_INT(0x01, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x08,
                             AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
                                 CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
                                 (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS |
                                           (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS |
                                           (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                 CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x01),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(
        AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
        AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(EPNUM_AUDIO_FB, 4, 1),
#undef _SPK_ITF

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "CONFIG_TOTAL_LEN mismatch");
TU_VERIFY_STATIC(TUD_AUDIO_SPEAKER_8CH_FB_DESC_LEN ==
                     CFG_TUD_AUDIO_FUNC_1_DESC_LEN,
                 "CFG_TUD_AUDIO_FUNC_1_DESC_LEN mismatch");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_AUDIO_ITF,
  STRID_CDC_ITF,
};

char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Freshwater",
    "Channel Card Audio",
    "CHCARD-001",
    /* CoreAudio uses this interface string as the output device name. */
    "Channel Card",
    "Channel Card Console",
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t chr_count;

  if (index == STRID_LANGID) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
      return NULL;

    const char *str = string_desc_arr[index];
    chr_count = strlen(str);
    size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
    if (chr_count > max_count)
      chr_count = max_count;

    for (size_t i = 0; i < chr_count; i++) {
      _desc_str[1 + i] = str[i];
    }
  }

  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
