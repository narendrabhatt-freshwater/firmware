/* Single CDC ACM function; no audio interfaces. */
#include "usb_device.h"
#include "usb_stream.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>
static const uint8_t device[] = {
  18,1,0,2,0xEF,2,1,64,
  USB_STREAM_VID & 255, USB_STREAM_VID >> 8,
  USB_STREAM_PID & 255, USB_STREAM_PID >> 8,
  0,3,1,2,3,1
};
static const uint8_t configuration[] = {
  9,2,75,0,2,1,0,0x80,50,
  8,11,0,2,2,2,1,0,
  9,4,0,0,1,2,2,1,0,
  5,0x24,0,0x10,1,
  5,0x24,1,0,1,
  4,0x24,2,2,
  5,0x24,6,0,1,
  7,5,0x82,3,8,0,16,
  9,4,1,0,2,0x0A,0,0,0,
  7,5,0x01,2,64,0,0,
  7,5,0x81,2,64,0,0
};
_Static_assert(sizeof(configuration) == 75, "CDC descriptor length");
const uint8_t *USB_Descriptor(uint8_t type, uint8_t index, uint16_t *size)
{
  static uint8_t string[66];
  char serial[32];
  const char *text;
  if (type == 1 && index == 0) { *size = sizeof(device); return device; }
  if (type == 2 && index == 0) { *size = sizeof(configuration); return configuration; }
  if (type != 3 || index > 3) return NULL;
  if (index == 0) { string[0]=4; string[1]=3; string[2]=9; string[3]=4; *size=4; return string; }
  (void)snprintf(serial, sizeof serial, "CHCARD-%08lX%08lX%08lX",
    (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
  text = index == 1 ? "Freshwater" : index == 2 ? "Channel Card Data" : serial;
  *size = (uint16_t)(2 + 2 * strlen(text));
  string[0] = (uint8_t)*size; string[1] = 3;
  for (unsigned i=0; text[i]; ++i) { string[2+2*i]=(uint8_t)text[i]; string[3+2*i]=0; }
  return string;
}
