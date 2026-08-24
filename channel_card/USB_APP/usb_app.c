/* USB application layer — Channel Card
 *
 * Vendor bulk BODY → stream rings. CDC console / attack load.
 *
 * USB OTG IRQ: tud_int_handler only. This file runs from main.
 */

#include "usb_app.h"
#include "audio_bridge.h"
#include "attack_upload.h"
#include "channel_console.h"
#include "main.h"
#include "note_bank.h"
#include "stream_ring.h"
#include "tusb.h"
#include "usb_stream.h"

#include <string.h>

static uint16_t s_last_pack_sequence = 0xFFFFu;

static void USB_LowLevel_Init(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInitStruct.PLL3.PLL3M = 4;
  PeriphClkInitStruct.PLL3.PLL3N = 125;
  PeriphClkInitStruct.PLL3.PLL3P = 16;
  PeriphClkInitStruct.PLL3.PLL3Q = 16;
  PeriphClkInitStruct.PLL3.PLL3R = 8;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 0;
  PeriphClkInitStruct.UsbClockSelection = RCC_USBCLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  HAL_PWREx_EnableUSBVoltageDetector();

  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

  /* DCD packet copy still has a 1 ms FS-frame deadline. BODY parse does
   * not run here — a late main loop NAKs instead of dropping a frame. */
  HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}

void USB_App_Init(void)
{
  USB_LowLevel_Init();
  tud_init(BOARD_TUD_RHPORT);
}

void tud_mount_cb(void)
{
  s_last_pack_sequence = 0xFFFFu;
  Audio_Bridge_Start();
}

void tud_umount_cb(void)
{
  Audio_Bridge_StreamStop();
}

void USB_CDC_WriteStr(const char *s)
{
  if (s == NULL || !tud_cdc_connected())
    return;
  uint32_t len = (uint32_t)strlen(s);
  uint32_t sent = 0;
  while (sent < len)
  {
    uint32_t n = tud_cdc_write(s + sent, len - sent);
    sent += n;
    tud_cdc_write_flush();
    if (n == 0)
    {
      return;
    }
  }
}

static void CDC_Console_Poll(void)
{
  static char line[64];
  static uint8_t len = 0;

  while (tud_cdc_available())
  {
    if (AttackUpload_IsActive() != 0u)
    {
      uint8_t tmp[256];
      uint32_t n = tud_cdc_read(tmp, sizeof tmp);
      if (n == 0u)
      {
        break;
      }
      (void)AttackUpload_Feed(tmp, n);
      continue;
    }

    char c;
    if (tud_cdc_read(&c, 1) == 0)
      break;

    if (c == '\r' || c == '\n')
    {
      USB_CDC_WriteStr("\r\n");
      if (len > 0)
      {
        line[len] = '\0';
        len = 0;
        Console_ExecFromUSB(line);
      }
    }
    else if (c == 0x08 || c == 0x7F)
    {
      if (len > 0)
      {
        len--;
        USB_CDC_WriteStr("\b \b");
      }
    }
    else if (len < sizeof(line) - 1 && c >= 0x20 && c < 0x7F)
    {
      if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
      line[len++] = c;
      char e[2] = {c, '\0'};
      USB_CDC_WriteStr(e);
    }
  }
}

typedef enum
{
  USB_RX_HEADER = 0,
  USB_RX_META,
  USB_RX_SAMPLES,
  USB_RX_DISCARD
} USB_RxState_t;

/* TinyUSB already owns the vendor RX FIFO. Keep only framing state here and
 * read PCM directly into an unpublished per-voice ring reservation. */
static struct
{
  USB_RxState_t state;
  UsbStreamHdr hdr;
  UsbStreamBodyMeta meta;
  StreamRing_Write_t write;
  uint32_t header_got;
  uint32_t meta_got;
  uint32_t payload_left;
  uint32_t sample_bytes_left;
} s_rx;
static uint32_t s_rx_msg;
static uint32_t s_rx_bytes;
static uint32_t s_bad;

static void USB_App_ResetRx(void)
{
  StreamRing_WriteAbort(&s_rx.write);
  memset(&s_rx, 0, sizeof s_rx);
  s_rx.state = USB_RX_HEADER;
}

static void USB_App_DiscardFrame(void)
{
  StreamRing_WriteAbort(&s_rx.write);
  if (s_rx.payload_left == 0u)
  {
    USB_App_ResetRx();
  }
  else
  {
    s_rx.state = USB_RX_DISCARD;
  }
}

static void USB_App_FinishFrame(void)
{
  if (s_rx.hdr.type == USB_STREAM_TYPE_PACK)
  {
    s_last_pack_sequence = s_rx.hdr.pad;
  }
  USB_App_ResetRx();
}

static void USB_App_DrainVendor(void)
{
  static uint8_t discard[64];

  if (!tud_vendor_mounted())
  {
    USB_App_ResetRx();
    return;
  }

  while (tud_vendor_available() != 0u)
  {
    uint32_t avail = tud_vendor_available();
    uint32_t n;

    if (s_rx.state == USB_RX_DISCARD)
    {
      n = s_rx.payload_left;
      if (n > avail)
      {
        n = avail;
      }
      if (n > sizeof discard)
      {
        n = sizeof discard;
      }
      n = tud_vendor_read(discard, n);
      if (n == 0u)
      {
        break;
      }
      s_rx.payload_left -= n;
      s_rx_bytes += n;
      if (s_rx.payload_left == 0u)
      {
        USB_App_ResetRx();
      }
      continue;
    }

    if (s_rx.state == USB_RX_HEADER)
    {
      uint8_t *header = (uint8_t *)(void *)&s_rx.hdr;
      n = USB_STREAM_HDR_SIZE - s_rx.header_got;
      if (n > avail)
      {
        n = avail;
      }
      n = tud_vendor_read(header + s_rx.header_got, n);
      if (n == 0u)
      {
        break;
      }
      s_rx.header_got += n;
      s_rx_bytes += n;
      if (s_rx.header_got < USB_STREAM_HDR_SIZE)
      {
        continue;
      }
      if (header[0] != USB_STREAM_MAGIC0 || header[1] != USB_STREAM_MAGIC1)
      {
        s_bad++;
        memmove(header, header + 1, USB_STREAM_HDR_SIZE - 1u);
        s_rx.header_got = USB_STREAM_HDR_SIZE - 1u;
        continue;
      }
      if (s_rx.hdr.nbytes > (USB_STREAM_MSG_MAX - USB_STREAM_HDR_SIZE))
      {
        s_bad++;
        USB_App_ResetRx();
        continue;
      }
      s_rx.payload_left = s_rx.hdr.nbytes;
      if (s_rx.hdr.type != USB_STREAM_TYPE_BODY &&
          s_rx.hdr.type != USB_STREAM_TYPE_PACK)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      if (s_rx.payload_left < USB_STREAM_BODY_META_SIZE)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      s_rx.state = USB_RX_META;
      s_rx.meta_got = 0u;
      continue;
    }

    if (s_rx.state == USB_RX_META)
    {
      uint8_t *meta = (uint8_t *)(void *)&s_rx.meta;
      n = USB_STREAM_BODY_META_SIZE - s_rx.meta_got;
      if (n > avail)
      {
        n = avail;
      }
      n = tud_vendor_read(meta + s_rx.meta_got, n);
      if (n == 0u)
      {
        break;
      }
      s_rx.meta_got += n;
      s_rx.payload_left -= n;
      s_rx_bytes += n;
      if (s_rx.meta_got < USB_STREAM_BODY_META_SIZE)
      {
        continue;
      }
      s_rx.sample_bytes_left = (uint32_t)s_rx.meta.nsamp * 2u;
      if (s_rx.meta.voice >= SAMPLE_VOICES || s_rx.meta.nsamp == 0u ||
          s_rx.meta.nsamp > USB_STREAM_NSAMP_MAX ||
          s_rx.sample_bytes_left > s_rx.payload_left ||
          (s_rx.hdr.type == USB_STREAM_TYPE_BODY &&
           s_rx.sample_bytes_left != s_rx.payload_left))
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      if (StreamRing_WriteBegin(s_rx.meta.voice, s_rx.meta.session,
                                s_rx.meta.sof, s_rx.meta.nsamp,
                                &s_rx.write) != 0)
      {
        /* Ring overflow is already counted as a dropped BODY burst. Do not
         * acknowledge a PACK that the host must reconcile/retry. */
        USB_App_DiscardFrame();
        continue;
      }
      s_rx.state = USB_RX_SAMPLES;
      continue;
    }

    if (s_rx.state == USB_RX_SAMPLES)
    {
      uint32_t span_samples = 0u;
      int16_t *span = StreamRing_WriteSpan(&s_rx.write, &span_samples);
      if (span == NULL || span_samples == 0u)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      n = span_samples * 2u;
      if (n > s_rx.sample_bytes_left)
      {
        n = s_rx.sample_bytes_left;
      }
      if (n > avail)
      {
        n = avail;
      }
      /* Valid PCM payloads and every FS packet boundary are 16-bit aligned. */
      n &= ~1u;
      if (n == 0u)
      {
        break;
      }
      n = tud_vendor_read(span, n);
      if (n == 0u || (n & 1u) != 0u ||
          StreamRing_WriteAdvance(&s_rx.write, n / 2u) != 0)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      s_rx.sample_bytes_left -= n;
      s_rx.payload_left -= n;
      s_rx_bytes += n;
      if (s_rx.sample_bytes_left != 0u)
      {
        continue;
      }
      if (StreamRing_WriteCommit(&s_rx.write) != s_rx.meta.nsamp)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      s_rx_msg++;
      if (s_rx.payload_left == 0u)
      {
        USB_App_FinishFrame();
        continue;
      }
      if (s_rx.hdr.type != USB_STREAM_TYPE_PACK ||
          s_rx.payload_left < USB_STREAM_BODY_META_SIZE)
      {
        s_bad++;
        USB_App_DiscardFrame();
        continue;
      }
      memset(&s_rx.meta, 0, sizeof s_rx.meta);
      s_rx.meta_got = 0u;
      s_rx.state = USB_RX_META;
    }
  }
}

void USB_App_Task(void)
{
  tud_task();
  USB_App_DrainVendor();
  CDC_Console_Poll();
}

uint16_t USB_App_LastPackSequence(void)
{
  return s_last_pack_sequence;
}

uint32_t USB_App_RxMsgCount(void)
{
  return s_rx_msg;
}

uint32_t USB_App_RxByteCount(void)
{
  return s_rx_bytes;
}

uint32_t USB_App_BadCount(void)
{
  return s_bad;
}

void USB_App_StatsClear(void)
{
  s_rx_msg = 0u;
  s_rx_bytes = 0u;
  s_bad = 0u;
}
