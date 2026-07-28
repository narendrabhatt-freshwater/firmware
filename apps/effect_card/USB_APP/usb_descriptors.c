/* USB descriptors — Effect Card composite device
 *   ITF0/1: UAC2 microphone (mono, 32-bit, 96 kHz)
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

    .idVendor = 0xCafe, /* TODO: real VID/PID before release */
    /* PID bumped again (0x4014 -> 0x4015): the audio EP size reverted to
     * the computed 388 B.  Windows caches descriptors per VID/PID, so a
     * changed endpoint under a reused PID serves stale data (see the
     * channel card volume saga).  Bump on ANY descriptor change. */
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
  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_MIC_ONE_CH_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_AUDIO_IN 0x01 /* iso IN 0x81                  */
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83

uint8_t const desc_configuration[] = {
    /* Config number, interface count, string index, total length,
     * attribute, power in mA (bus-powered budget) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* UAC2 microphone: itf, stridx, bytes/sample, bits/sample, EP, EP size */
    TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR(
        /*_itfnum*/ ITF_NUM_AUDIO_CONTROL, /*_stridx*/ 4,
        /*_nBytesPerSample*/ CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,
        /*_nBitsUsedPerSample*/ 32,
        /*_epin*/ 0x80 | EPNUM_AUDIO_IN,
        /*_epsize*/ CFG_TUD_AUDIO_EP_SZ_IN),

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
    "Effect Card Audio",        /* 2: Product               */
    "EFCARD-001",               /* 3: Serial                */
    "ADC Monitor 96k/32",       /* 4: Audio interface       */
    "Effect Card Console",      /* 5: CDC interface         */
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
