/* Channel Card direct tagged UAC2 BODY transport + CDC console. */

#include "usb_app.h"
#include "audio_bridge.h"
#include "attack_upload.h"
#include "channel_console.h"
#include "vm_upload.h"
#include "main.h"
#include "stream_ring.h"
#include "tusb.h"
#include "usb_stream.h"

#include <stdbool.h>
#include <string.h>

static uint32_t s_sample_freq = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
static uint8_t s_clock_valid = 1u;
static audio_control_range_4_n_t(1) s_sample_freq_range;
static volatile uint8_t s_tud_from_isr;

static uint32_t s_rx_msg;
static uint32_t s_rx_bytes;
static uint32_t s_uac_windows;
static uint8_t s_uac_packet[USB_STREAM_UAC_PACKET_BYTES]
    __attribute__((aligned(2)));
static int16_t s_uac_logical[USB_STREAM_UAC_PACKET_WORDS];
static uint16_t s_uac_logical_got;
static uint8_t s_uac_synced;
#define USB_PACKET_LENGTH_QUEUE 16u
static volatile uint16_t s_uac_lengths[USB_PACKET_LENGTH_QUEUE];
static volatile uint8_t s_uac_length_wr;
static volatile uint8_t s_uac_length_rd;
static volatile uint32_t s_bad_uac;

static void USB_App_ResetUacAlignment(void)
{
  s_uac_length_rd = s_uac_length_wr;
  s_uac_logical_got = 0u;
  s_uac_synced = 0u;
}

static void USB_App_ConsumeUacWords(const int16_t *words, uint16_t nwords)
{
  uint16_t at = 0u;
  if (s_uac_synced == 0u)
  {
    /* CoreAudio begins on an audio-frame boundary, but not necessarily on
     * the endpoint's millisecond boundary. Find the existing tag once; from
     * there the 510-word period is exact and no further scanning is needed. */
    for (at = 0u; at < nwords; at = (uint16_t)(at + USB_STREAM_UAC_CHANNELS))
    {
      const uint16_t tag = (uint16_t)words[at];
      if (tag == USB_STREAM_TAG_IDLE ||
          (tag & USB_STREAM_TAG_MASK) == USB_STREAM_TAG_BASE)
      {
        s_uac_synced = 1u;
        break;
      }
    }
    if (s_uac_synced == 0u)
      return;
  }
  while (at < nwords)
  {
    uint16_t copy_n = (uint16_t)(USB_STREAM_UAC_PACKET_WORDS -
                                 s_uac_logical_got);
    if (copy_n > (uint16_t)(nwords - at))
      copy_n = (uint16_t)(nwords - at);
    memcpy(s_uac_logical + s_uac_logical_got, words + at,
           copy_n * sizeof(int16_t));
    s_uac_logical_got = (uint16_t)(s_uac_logical_got + copy_n);
    at = (uint16_t)(at + copy_n);
    if (s_uac_logical_got == USB_STREAM_UAC_PACKET_WORDS)
    {
      s_rx_msg += StreamRing_WriteUac(s_uac_logical);
      s_uac_windows++;
      s_uac_logical_got = 0u;
    }
  }
}

static void USB_LowLevel_Init(void)
{
  RCC_PeriphCLKInitTypeDef clock = {0};
  clock.PeriphClockSelection = RCC_PERIPHCLK_USB;
  clock.PLL3.PLL3M = 4;
  clock.PLL3.PLL3N = 125;
  clock.PLL3.PLL3P = 16;
  clock.PLL3.PLL3Q = 16;
  clock.PLL3.PLL3R = 8;
  clock.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  clock.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  clock.PLL3.PLL3FRACN = 0;
  clock.UsbClockSelection = RCC_USBCLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_PWREx_EnableUSBVoltageDetector();
  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
  HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}

void USB_App_Init(void)
{
  s_sample_freq_range.wNumSubRanges = 1;
  s_sample_freq_range.subrange[0].bMin = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
  s_sample_freq_range.subrange[0].bMax = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
  s_sample_freq_range.subrange[0].bRes = 0;
  USB_LowLevel_Init();
  if (!tud_init(BOARD_TUD_RHPORT))
    Error_Handler();
}

void USB_CDC_WriteStr(const char *s)
{
  uint32_t sent = 0u;
  uint32_t len;
  if (s == NULL || !tud_cdc_connected())
    return;
  len = (uint32_t)strlen(s);
  while (sent < len)
  {
    uint32_t n = tud_cdc_write(s + sent, len - sent);
    sent += n;
    tud_cdc_write_flush();
    if (n == 0u)
      return;
  }
}

static void CDC_Console_Poll(void)
{
  static char line[64];
  static uint8_t len;
  if (!tud_cdc_connected())
  {
    if (VmUpload_IsActive() != 0u) VmUpload_Abort();
    if (AttackUpload_IsActive() != 0u) AttackUpload_Abort();
    len = 0u;
    return;
  }
  while (tud_cdc_available())
  {
    char c;
    if (VmUpload_IsActive() != 0u)
    {
      uint8_t tmp[256];
      uint32_t n = tud_cdc_read(tmp, sizeof tmp);
      if (n == 0u)
        break;
      (void)VmUpload_Feed(tmp, n);
      continue;
    }
    if (AttackUpload_IsActive() != 0u)
    {
      uint8_t tmp[256];
      uint32_t n = tud_cdc_read(tmp, sizeof tmp);
      if (n == 0u)
        break;
      (void)AttackUpload_Feed(tmp, n);
      continue;
    }
    if (tud_cdc_read(&c, 1) == 0u)
      break;
    if (c == '\r' || c == '\n')
    {
      USB_CDC_WriteStr("\r\n");
      if (len != 0u)
      {
        line[len] = '\0';
        len = 0u;
        Console_ExecFromUSB(line);
      }
    }
    else if (c == 0x08 || c == 0x7F)
    {
      if (len != 0u)
      {
        len--;
        USB_CDC_WriteStr("\b \b");
      }
    }
    else if (len < sizeof(line) - 1u && c >= 0x20 && c < 0x7F)
    {
      if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
      line[len++] = c;
      {
        char echo[2] = {c, '\0'};
        USB_CDC_WriteStr(echo);
      }
    }
  }
}

static void USB_App_DrainUac(void)
{
  if (!tud_audio_mounted())
  {
    USB_App_ResetUacAlignment();
    return;
  }
  /* The ISR callback records each physical ISO OUT transfer length. Consume
   * exactly one such transfer here so FIFO batching cannot erase the USB
   * millisecond boundary that carries the tag. */
  if (s_uac_length_rd != s_uac_length_wr)
  {
    const uint8_t rd = s_uac_length_rd;
    const uint16_t packet_bytes = s_uac_lengths[rd];
    if (tud_audio_available() < packet_bytes)
      return;
    if (packet_bytes == USB_STREAM_UAC_PACKET_BYTES)
    {
      const uint16_t n = tud_audio_read(s_uac_packet, packet_bytes);
      if (n != packet_bytes)
      {
        s_bad_uac++;
        Error_Handler();
      }
      s_rx_bytes += n;
      USB_App_ConsumeUacWords(
          (const int16_t *)(const void *)s_uac_packet,
          USB_STREAM_UAC_PACKET_WORDS);
    }
    else
    {
      uint16_t left = packet_bytes;
      while (left != 0u)
      {
        uint16_t chunk = left;
        if (chunk > sizeof s_uac_packet)
          chunk = sizeof s_uac_packet;
        chunk = tud_audio_read(s_uac_packet, chunk);
        if (chunk == 0u)
        {
          s_bad_uac++;
          Error_Handler();
        }
        left = (uint16_t)(left - chunk);
        s_rx_bytes += chunk;
      }
      s_bad_uac++;
      /* A malformed ISO transfer destroys the fixed BODY framing. */
      Error_Handler();
    }
    s_uac_length_rd = (uint8_t)((rd + 1u) % USB_PACKET_LENGTH_QUEUE);
  }
}

void USB_App_TaskFromIsr(void)
{
  if (s_tud_from_isr != 0u)
    return;
  s_tud_from_isr = 1u;
  tud_task();
  s_tud_from_isr = 0u;
}

void USB_App_Task(void)
{
  USB_App_DrainUac();
  CDC_Console_Poll();
}

uint16_t USB_App_LastPackSequence(void) { return 0xFFFFu; }
uint32_t USB_App_RxMsgCount(void) { return s_rx_msg; }
uint32_t USB_App_RxByteCount(void) { return s_rx_bytes; }
uint32_t USB_App_UacWindowCount(void) { return s_uac_windows; }
uint32_t USB_App_BadCount(void) { return s_bad_uac; }
uint32_t USB_App_BadReasonCount(uint8_t reason)
{
  return reason == 4u ? s_bad_uac : 0u;
}

void USB_App_StatsClear(void)
{
  s_rx_msg = 0u;
  s_rx_bytes = 0u;
  s_uac_windows = 0u;
  s_bad_uac = 0u;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request,
                             uint8_t *buffer)
{
  (void)rhport; (void)request; (void)buffer;
  return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *request,
                              uint8_t *buffer)
{
  (void)rhport; (void)request; (void)buffer;
  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
  (void)rhport; (void)request; (void)buffer;
  return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request)
{
  (void)rhport; (void)request;
  return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *request)
{
  (void)rhport; (void)request;
  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request)
{
  uint8_t control = TU_U16_HIGH(request->wValue);
  uint8_t entity = TU_U16_HIGH(request->wIndex);
  if (entity == 1u && control == AUDIO_TE_CTRL_CONNECTOR)
  {
    audio_desc_channel_cluster_t result;
    result.bNrChannels = CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX;
    result.bmChannelConfig = (audio_channel_config_t)0;
    result.iChannelNames = 0;
    return tud_audio_buffer_and_schedule_control_xfer(
        rhport, request, &result, sizeof result);
  }
  if (entity == 4u)
  {
    if (control == AUDIO_CS_CTRL_SAM_FREQ)
    {
      if (request->bRequest == AUDIO_CS_REQ_CUR)
        return tud_control_xfer(rhport, request, &s_sample_freq,
                                sizeof s_sample_freq);
      if (request->bRequest == AUDIO_CS_REQ_RANGE)
        return tud_control_xfer(rhport, request, &s_sample_freq_range,
                                sizeof s_sample_freq_range);
    }
    if (control == AUDIO_CS_CTRL_CLK_VALID)
      return tud_control_xfer(rhport, request, &s_clock_valid,
                              sizeof s_clock_valid);
  }
  return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *request)
{
  (void)rhport;
  USB_App_ResetUacAlignment();
  if ((uint8_t)tu_le16toh(request->wValue) != 0u)
    Audio_Bridge_Start();
  return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const *request)
{
  (void)rhport; (void)request;
  USB_App_ResetUacAlignment();
  Audio_Bridge_StreamStop();
  return true;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t nbytes,
                                    uint8_t func_id, uint8_t ep_out,
                                    uint8_t alt)
{
  uint8_t next;
  (void)rhport; (void)func_id; (void)ep_out; (void)alt;
  next = (uint8_t)((s_uac_length_wr + 1u) % USB_PACKET_LENGTH_QUEUE);
  if (next == s_uac_length_rd)
  {
    s_bad_uac++;
    /* Lost boundary metadata makes the buffered audio ambiguous. */
    Error_Handler();
    return true;
  }
  s_uac_lengths[s_uac_length_wr] = nbytes;
  s_uac_length_wr = next;
  return true;
}
