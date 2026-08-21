/* USB descriptors — Channel Card composite
 *   ITF0: vendor bulk BODY (OUT 0x01 / IN 0x81, FS MPS 64)
 *   ITF1/2: CDC-ACM console
 *
 * Vendor DCD buffer is CFG_TUD_VENDOR_EPSIZE (multi-packet). wMaxPacketSize
 * here must stay 64 on Full-Speed.
 *
 * PID bump on any descriptor change (macOS caches by VID/PID).
 * 0x4019 = UAC2; 0x4020 = vendor bulk; 0x4021 = vendor ISO (withdrawn:
 * macOS panics); 0x4022 = vendor bulk PACK + CDC.
 */

#include "tusb.h"
#include "usb_stream.h"

#include <string.h>

#ifndef TUD_VENDOR_DESC_LEN
#define TUD_VENDOR_DESC_LEN (9 + 7 + 7)
#endif

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,

    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_STREAM_VID,
    .idProduct = USB_STREAM_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

enum {
  ITF_NUM_VENDOR = 0,
  ITF_NUM_CDC,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN                                                       \
  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_VENDOR_OUT 0x01
#define EPNUM_VENDOR_IN 0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT 0x03
#define EPNUM_CDC_IN 0x83

#define USB_STREAM_FS_MPS 64

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    9, TUSB_DESC_INTERFACE, ITF_NUM_VENDOR, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC,
    0x00, 0x00, 4,
    7, TUSB_DESC_ENDPOINT, EPNUM_VENDOR_OUT, TUSB_XFER_BULK,
    U16_TO_U8S_LE(USB_STREAM_FS_MPS), 0,
    7, TUSB_DESC_ENDPOINT, EPNUM_VENDOR_IN, TUSB_XFER_BULK,
    U16_TO_U8S_LE(USB_STREAM_FS_MPS), 0,

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "CONFIG_TOTAL_LEN mismatch");
TU_VERIFY_STATIC(USB_STREAM_FS_MPS == 64, "FS bulk MPS must be 64");
TU_VERIFY_STATIC(sizeof(UsbStreamHdr) == USB_STREAM_HDR_SIZE, "hdr size");
TU_VERIFY_STATIC(sizeof(UsbStreamBodyMeta) == USB_STREAM_BODY_META_SIZE,
                 "body meta size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

enum { VENDOR_REQUEST_MICROSOFT = 1 };

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2

uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void) { return desc_bos; }

uint8_t const desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W',
    'I', 'N', 'U', 'S', 'B', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,

    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0,
    'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, 'F', 0, '5', 0, '7', 0, '4', 0, 'A', 0, 'D', 0, 'A', 0, '7', 0,
    '-', 0, '3', 0, '0', 0, '8', 0, '3', 0, '-', 0, '4', 0, '9', 0, '8', 0,
    'C', 0, '-', 0, '8', 0, 'C', 0, '4', 0, '2', 0, '-', 0, 'A', 0, '3', 0,
    '2', 0, '1', 0, '5', 0, '6', 0, '5', 0, 'F', 0, '5', 0, '8', 0, '5', 0,
    '9', 0, '}', 0, 0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN,
                 "MS OS 2.0 length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) {
    return true;
  }
  if (request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7) {
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20,
                            sizeof desc_ms_os_20);
  }
  return false;
}

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_VENDOR_ITF,
  STRID_CDC_ITF,
};

char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Freshwater",
    "Channel Card Audio",
    "CHCARD-001",
    "Channel Card Stream",
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
