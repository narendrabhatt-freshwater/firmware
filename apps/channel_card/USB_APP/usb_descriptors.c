/* USB descriptors — Channel Card composite device
 *   ITF0/1: UAC2 speaker (mono, 32-bit, 96 kHz) + feedback EP
 *   ITF2/3: CDC-ACM debug console
 */

#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    /* IAD composite: class/subclass/protocol must be Misc/Common/IAD */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    /* Distinct from the Effect Card (0xCafe/0x4011) so Windows never
     * cross-binds drivers between the two boards.  Note: 0x0483/0x5740 —
     * ST's Virtual COM Port ID — must be avoided; using it previously made
     * Windows force the VCP driver onto the audio device. */
    .idVendor = 0xCafe,
    /* PID history: 0x4012→0x4013 (mute-only FU), 0x4013→0x4014 (48 kHz),
     * 0x4014→0x4015 (96 kHz). Windows caches UAC descriptors per VID/PID —
     * bump on further descriptor changes during bring-up. */
    .idProduct = 0x4015,
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
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_AUDIO_OUT 0x01 /* iso OUT 0x01           */
#define EPNUM_AUDIO_FB 0x81  /* iso IN  0x81 (feedback) */
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83

uint8_t const desc_configuration[] = {
    /* Config number, interface count, string index, total length,
     * attribute, power in mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* UAC2 mono speaker with explicit feedback endpoint.
     *
     * This is TUD_AUDIO_SPEAKER_MONO_FB_DESCRIPTOR() expanded inline with
     * ONE change: the Feature Unit advertises MUTE only, NOT volume.
     *
     * Why: if we advertise a hardware volume control, Windows pushes one
     * SET at enumeration and then does all runtime volume in software —
     * leaving the device permanently attenuated at that stale value while
     * the slider does nothing.  Advertising mute-only (exactly like the
     * old ST UAC1 firmware that worked) makes Windows scale the PCM in
     * software for the whole range, and the device passes samples through.
     * The descriptor length is unchanged (only control bits differ), so
     * TUD_AUDIO_SPEAKER_MONO_FB_DESC_LEN / CONFIG_TOTAL_LEN stay valid. */
#define _SPK_ITF ITF_NUM_AUDIO_CONTROL
    /* IAD */
    TUD_AUDIO_DESC_IAD(_SPK_ITF, 0x02, 0x00),
    /* Standard AC Interface */
    TUD_AUDIO_DESC_STD_AC(_SPK_ITF, 0x00, 4),
    /* Class-Specific AC Interface Header */
    TUD_AUDIO_DESC_CS_AC(0x0200, AUDIO_FUNC_DESKTOP_SPEAKER,
                         TUD_AUDIO_DESC_CLK_SRC_LEN +
                             TUD_AUDIO_DESC_INPUT_TERM_LEN +
                             TUD_AUDIO_DESC_OUTPUT_TERM_LEN +
                             TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL_LEN,
                         AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),
    /* Clock Source */
    TUD_AUDIO_DESC_CLK_SRC(0x04, AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                           (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS),
                           0x01, 0x00),
    /* Input Terminal (USB streaming) */
    TUD_AUDIO_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x04,
                              0x01, AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00,
                              0 * (AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS),
                              0x00),
    /* Output Terminal (speaker) */
    TUD_AUDIO_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x01,
                               0x02, 0x04, 0x0000, 0x00),
    /* Feature Unit — MUTE only (no volume control advertised) */
    TUD_AUDIO_DESC_FEATURE_UNIT_ONE_CHANNEL(
        0x02, 0x01,
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS),
        (AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS), 0x00),
    /* Standard AS Interface, Alt 0 (zero-bandwidth) */
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(_SPK_ITF + 1), 0x00, 0x00, 0x00),
    /* Standard AS Interface, Alt 1 (streaming) */
    TUD_AUDIO_DESC_STD_AS_INT((uint8_t)(_SPK_ITF + 1), 0x01, 0x02, 0x00),
    /* Class-Specific AS Interface */
    TUD_AUDIO_DESC_CS_AS_INT(0x01, AUDIO_CTRL_NONE, AUDIO_FORMAT_TYPE_I,
                             AUDIO_DATA_FORMAT_TYPE_I_PCM, 0x01,
                             AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    /* Type I Format */
    TUD_AUDIO_DESC_TYPE_I_FORMAT(CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX,
                                 CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX),
    /* Standard AS Iso Data EP (OUT, async) */
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
                                 (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS |
                                           (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS |
                                           (uint8_t)TUSB_ISO_EP_ATT_DATA),
                                 CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x01),
    /* Class-Specific AS Iso Data EP */
    TUD_AUDIO_DESC_CS_AS_ISO_EP(
        AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO_CTRL_NONE,
        AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    /* Standard AS Iso Feedback EP */
    TUD_AUDIO_DESC_STD_AS_ISO_FB_EP(EPNUM_AUDIO_FB, 4, 1),
#undef _SPK_ITF

    /* CDC: itf, stridx, notif EP, notif size, data OUT EP, data IN EP, size */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),
};

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
    (const char[]){0x09, 0x04}, /* 0: English (0x0409)      */
    "Freshwater",               /* 1: Manufacturer          */
    "Channel Card Audio",       /* 2: Product               */
    "CHCARD-001",               /* 3: Serial                */
    "DAC Out 96k/32",           /* 4: Audio interface       */
    "Channel Card Console",     /* 5: CDC interface         */
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

  /* first element: length (bytes) + descriptor type */
  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
