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

static volatile uint32_t s_status_frame;
static uint32_t s_status_sent_frame = 0xFFFFFFFFu;
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
  s_status_sent_frame = 0xFFFFFFFFu;
  s_last_pack_sequence = 0xFFFFu;
  tud_sof_cb_enable(true);
  Audio_Bridge_Start();
}

void tud_umount_cb(void)
{
  tud_sof_cb_enable(false);
  Audio_Bridge_StreamStop();
}

void tud_sof_cb(uint32_t frame_count)
{
  s_status_frame = frame_count;
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

/* The packed BODY assembly is too large for the nearly-full DTCM .bss.
 * AXI SRAM is CPU-visible and already owns the USB/DMA working buffers. */
static uint8_t s_msg[USB_STREAM_MSG_MAX]
    __attribute__((aligned(4), section(".dma_buffer")));
static uint32_t s_got;
static uint32_t s_need;
static uint32_t s_rx_msg;
static uint32_t s_rx_bytes;
static uint32_t s_bad;
static uint32_t s_status_tx;
static void USB_App_ResetAsm(void)
{
  s_got = 0u;
  s_need = 0u;
}

static void USB_App_HandleBody(const uint8_t *payload, uint16_t nbytes)
{
  const UsbStreamBodyMeta *meta;
  const int16_t *samples;

  if (nbytes < USB_STREAM_BODY_META_SIZE)
  {
    s_bad++;
    return;
  }
  meta = (const UsbStreamBodyMeta *)(const void *)payload;
  if (meta->nsamp == 0u || meta->nsamp > USB_STREAM_NSAMP_MAX)
  {
    s_bad++;
    return;
  }
  if ((uint32_t)nbytes !=
      (USB_STREAM_BODY_META_SIZE + ((uint32_t)meta->nsamp * 2u)))
  {
    s_bad++;
    return;
  }
  samples = (const int16_t *)(const void *)(payload + USB_STREAM_BODY_META_SIZE);
  (void)StreamRing_WriteVoice(meta->voice, meta->session, meta->sof, samples,
                              meta->nsamp);
}

static void USB_App_HandlePack(const uint8_t *payload, uint16_t nbytes)
{
  uint32_t po = 0u;
  while ((po + USB_STREAM_BODY_META_SIZE) <= (uint32_t)nbytes)
  {
    UsbStreamBodyMeta meta;
    uint32_t chunk;

    memcpy(&meta, payload + po, sizeof meta);
    chunk = USB_STREAM_BODY_META_SIZE + ((uint32_t)meta.nsamp * 2u);
    if (meta.nsamp == 0u || meta.nsamp > USB_STREAM_NSAMP_MAX ||
        (po + chunk) > (uint32_t)nbytes)
    {
      s_bad++;
      return;
    }
    USB_App_HandleBody(payload + po, (uint16_t)chunk);
    po += chunk;
    s_rx_msg++;
  }
}

static void USB_App_DrainVendor(void)
{
  if (!tud_vendor_mounted())
  {
    USB_App_ResetAsm();
    return;
  }

  for (;;)
  {
    uint32_t avail = tud_vendor_available();
    uint32_t n;

    if (avail == 0u)
    {
      break;
    }

    if (s_got < USB_STREAM_HDR_SIZE)
    {
      n = USB_STREAM_HDR_SIZE - s_got;
      if (n > avail)
      {
        n = avail;
      }
      n = tud_vendor_read(s_msg + s_got, n);
      if (n == 0u)
      {
        break;
      }
      s_got += n;
      s_rx_bytes += n;
      if (s_got < USB_STREAM_HDR_SIZE)
      {
        break;
      }
      if (s_msg[0] != USB_STREAM_MAGIC0 || s_msg[1] != USB_STREAM_MAGIC1)
      {
        s_bad++;
        memmove(s_msg, s_msg + 1, USB_STREAM_HDR_SIZE - 1u);
        s_got = USB_STREAM_HDR_SIZE - 1u;
        continue;
      }
      {
        const UsbStreamHdr *hdr = (const UsbStreamHdr *)(const void *)s_msg;
        if (hdr->nbytes > (USB_STREAM_MSG_MAX - USB_STREAM_HDR_SIZE))
        {
          s_bad++;
          USB_App_ResetAsm();
          continue;
        }
        s_need = USB_STREAM_HDR_SIZE + (uint32_t)hdr->nbytes;
      }
    }

    if (s_got < s_need)
    {
      avail = tud_vendor_available();
      if (avail == 0u)
      {
        break;
      }
      n = s_need - s_got;
      if (n > avail)
      {
        n = avail;
      }
      n = tud_vendor_read(s_msg + s_got, n);
      if (n == 0u)
      {
        break;
      }
      s_got += n;
      s_rx_bytes += n;
    }

    if (s_got >= s_need && s_need >= USB_STREAM_HDR_SIZE)
    {
      const UsbStreamHdr *hdr = (const UsbStreamHdr *)(const void *)s_msg;
      if (hdr->type == USB_STREAM_TYPE_BODY)
      {
        USB_App_HandleBody(s_msg + USB_STREAM_HDR_SIZE, hdr->nbytes);
        s_rx_msg++;
      }
      else if (hdr->type == USB_STREAM_TYPE_PACK)
      {
        USB_App_HandlePack(s_msg + USB_STREAM_HDR_SIZE, hdr->nbytes);
        /* Publish only after every BODY in this PACK has been applied. The
         * host uses this acknowledgement to decide whether an OUT transfer
         * was already reflected by a racing exact-free-space snapshot. */
        s_last_pack_sequence = hdr->pad;
      }
      else
      {
        s_bad++;
      }
      USB_App_ResetAsm();
    }
  }
}

static void USB_App_PushStatus(void)
{
  uint8_t slots[NOTE_BANK_VOICES];
  uint8_t i;
  struct
  {
    UsbStreamHdr hdr;
    UsbStreamStatus status;
  } msg;

  const uint32_t frame = s_status_frame;
  if ((frame % USB_STREAM_STATUS_FRAME_PERIOD) != 0u ||
      frame == s_status_sent_frame || !tud_vendor_mounted() ||
      tud_vendor_write_available() < (uint32_t)sizeof msg)
  {
    return;
  }
  msg.hdr.magic0 = USB_STREAM_MAGIC0;
  msg.hdr.magic1 = USB_STREAM_MAGIC1;
  msg.hdr.type = USB_STREAM_TYPE_STATUS;
  msg.hdr.flags = 0u;
  msg.hdr.nbytes = (uint16_t)sizeof msg.status;
  msg.hdr.pad = 0u;
  NoteBank_VoiceQuery(&msg.status.mask, &msg.status.best, slots);
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    msg.status.free_samples[i] =
        (uint16_t)(STREAM_RING_SAMPLES - StreamRing_FillLevel(i));
  }
  msg.status.last_pack_sequence = s_last_pack_sequence;
  if (tud_vendor_write(&msg, sizeof msg) == sizeof msg)
  {
    s_status_sent_frame = frame;
    s_status_tx++;
    (void)tud_vendor_write_flush();
  }
}

void USB_App_Task(void)
{
  tud_task();
  USB_App_DrainVendor();
  USB_App_PushStatus();
  CDC_Console_Poll();
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

uint32_t USB_App_StatusCount(void)
{
  return s_status_tx;
}

void USB_App_StatsClear(void)
{
  s_rx_msg = 0u;
  s_rx_bytes = 0u;
  s_bad = 0u;
  s_status_tx = 0u;
}
