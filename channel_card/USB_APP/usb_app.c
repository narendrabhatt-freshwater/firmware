/* Channel Card UAC2 BODY transport + CDC console. */

#include "usb_app.h"
#include "audio_bridge.h"
#include "attack_upload.h"
#include "channel_console.h"
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

static uint16_t s_last_pack_sequence = 0xFFFFu;
static uint32_t s_rx_msg;
static uint32_t s_rx_bytes;
static uint32_t s_uac_windows;
static uint32_t s_uac_audio_frames;
static uint32_t s_bad;
enum
{
  USB_BAD_HEADER = 0,
  USB_BAD_SEQUENCE,
  USB_BAD_FRAME,
  USB_BAD_CRC,
  USB_BAD_UAC,
  USB_BAD_REASON_COUNT
};
static uint32_t s_bad_reason[USB_BAD_REASON_COUNT];
static uint8_t s_uac_packet[USB_STREAM_UAC_PACKET_BYTES];
static uint16_t s_uac_pending_offset;
static uint16_t s_uac_pending_bytes;
#define USB_STREAM_FUTURE_WAIT_MS 20u

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
  tud_init(BOARD_TUD_RHPORT);
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
  while (tud_cdc_available())
  {
    char c;
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

typedef enum
{
  USB_RX_HEADER = 0,
  USB_RX_META,
  USB_RX_SAMPLES,
  USB_RX_CRC,
  USB_RX_DISCARD
} USB_RxState_t;

static struct
{
  USB_RxState_t state;
  UsbStreamHdr hdr;
  UsbStreamBodyMeta meta;
  StreamRing_Write_t writes[8];
  uint8_t write_count;
  uint8_t current_write;
  uint8_t voice_mask;
  uint8_t stale_write_mask;
  uint32_t header_got;
  uint32_t meta_got;
  uint32_t payload_left;
  uint32_t sample_bytes_left;
  uint32_t crc;
  uint8_t crc_bytes[USB_STREAM_CRC_BYTES];
  uint8_t crc_got;
  uint8_t future_waiting;
  uint32_t future_since_ms;
} s_rx;

static struct
{
  uint8_t active;
  uint16_t pack_sequence;
} s_transport;

static void USB_App_AbortReservations(void)
{
  uint8_t i;
  for (i = 0u; i < s_rx.write_count; ++i)
    StreamRing_WriteAbort(&s_rx.writes[i]);
}

static void USB_App_ResetRx(void)
{
  USB_App_AbortReservations();
  memset(&s_rx, 0, sizeof s_rx);
  s_rx.state = USB_RX_HEADER;
}

static void USB_App_AbortTransport(void)
{
  USB_App_ResetRx();
  memset(&s_transport, 0, sizeof s_transport);
}

static void USB_App_RecordBad(uint8_t reason)
{
  s_bad++;
  if (reason < USB_BAD_REASON_COUNT)
    s_bad_reason[reason]++;
}

static void USB_App_FailFrame(uint8_t reason)
{
  USB_App_RecordBad(reason);
  USB_App_AbortTransport();
}

static uint32_t USB_App_Crc32Update(uint32_t crc, const uint8_t *src,
                                    uint32_t count)
{
  /* Reflected IEEE CRC32, identical to eight rounds of polynomial
   * 0xEDB88320 per byte. Two four-bit table rounds leave enough main-loop
   * time for the RS485 request/serve path at the full BODY wire budget. */
  static const uint32_t nibble[16] = {
      0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
      0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
      0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
      0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
  while (count-- != 0u)
  {
    crc ^= *src++;
    crc = (crc >> 4u) ^ nibble[crc & 0x0Fu];
    crc = (crc >> 4u) ^ nibble[crc & 0x0Fu];
  }
  return crc;
}

static int USB_App_BeginMetaWrite(void)
{
  int begin_result = StreamRing_WriteBegin(
      s_rx.meta.voice, s_rx.meta.session,
      (s_rx.meta.flags & USB_STREAM_BODY_FLAG_SOF) != 0u,
      s_rx.meta.wave_id, s_rx.meta.nsamp,
      &s_rx.writes[s_rx.write_count]);
  if (begin_result == STREAM_RING_WRITE_FUTURE)
  {
    uint32_t now = HAL_GetTick();
    if (s_rx.future_waiting == 0u)
    {
      s_rx.future_waiting = 1u;
      s_rx.future_since_ms = now;
    }
    if ((uint32_t)(now - s_rx.future_since_ms) <
        USB_STREAM_FUTURE_WAIT_MS)
      return 0;
    begin_result = STREAM_RING_WRITE_STALE;
  }
  if (begin_result == STREAM_RING_WRITE_PENDING)
    return 0;
  if (begin_result == STREAM_RING_WRITE_ERROR)
  {
    USB_App_FailFrame(USB_BAD_FRAME);
    return -1;
  }
  s_rx.current_write = s_rx.write_count;
  s_rx.future_waiting = 0u;
  if (begin_result == STREAM_RING_WRITE_STALE)
    s_rx.stale_write_mask |= (uint8_t)(1u << s_rx.write_count);
  s_rx.write_count++;
  s_rx.voice_mask |= (uint8_t)(1u << s_rx.meta.voice);
  s_rx.state = USB_RX_SAMPLES;
  return 1;
}

static int USB_App_FinishFrame(void)
{
  uint8_t i;
  uint8_t applied = 0u;
  if (s_rx.hdr.type != USB_STREAM_TYPE_PACK || s_rx.payload_left != 0u ||
      s_rx.write_count == 0u ||
      s_rx.hdr.pad != s_transport.pack_sequence)
  {
    USB_App_FailFrame(USB_BAD_FRAME);
    return -1;
  }
  for (i = 0u; i < s_rx.write_count; ++i)
  {
    if ((s_rx.stale_write_mask & (uint8_t)(1u << i)) != 0u)
      continue;
    if (s_rx.writes[i].active == 0u ||
        s_rx.writes[i].written != s_rx.writes[i].nsamp)
    {
      USB_App_FailFrame(USB_BAD_FRAME);
      return -1;
    }
    if (StreamRing_WriteIsCurrent(&s_rx.writes[i]) == 0u)
      s_rx.stale_write_mask |= (uint8_t)(1u << i);
  }
  /* No other producer can move wr between reservation and this commit loop. */
  for (i = 0u; i < s_rx.write_count; ++i)
  {
    if ((s_rx.stale_write_mask & (uint8_t)(1u << i)) != 0u)
    {
      StreamRing_WriteAbort(&s_rx.writes[i]);
      continue;
    }
    if (StreamRing_WriteCommit(&s_rx.writes[i]) == 0u)
    {
      USB_App_FailFrame(USB_BAD_FRAME);
      return -1;
    }
    applied++;
  }
  s_rx_msg += applied;
  s_last_pack_sequence = s_rx.hdr.pad;
  memset(&s_transport, 0, sizeof s_transport);
  memset(&s_rx, 0, sizeof s_rx);
  s_rx.state = USB_RX_HEADER;
  return 0;
}

static uint32_t USB_App_FeedPack(const uint8_t *src, uint32_t count)
{
  const uint32_t initial = count;
  while (count != 0u)
  {
    uint32_t n;
    if (s_rx.state == USB_RX_DISCARD)
    {
      n = s_rx.payload_left;
      if (n > count)
        n = count;
      s_rx.payload_left -= n;
      src += n;
      count -= n;
      if (s_rx.payload_left == 0u)
        USB_App_AbortTransport();
      continue;
    }
    if (s_rx.state == USB_RX_HEADER)
    {
      uint8_t *dst = (uint8_t *)(void *)&s_rx.hdr;
      uint16_t expected;
      dst[s_rx.header_got++] = *src++;
      count--;
      if (s_rx.header_got >= 4u &&
          (dst[0] != USB_STREAM_MAGIC0 ||
           dst[1] != USB_STREAM_MAGIC1 ||
           dst[2] != USB_STREAM_TYPE_PACK || dst[3] != 0u))
      {
        memmove(dst, dst + 1u, s_rx.header_got - 1u);
        s_rx.header_got--;
        continue;
      }
      if (s_rx.header_got < USB_STREAM_HDR_SIZE)
        continue;
      if (s_rx.hdr.nbytes < USB_STREAM_BODY_META_SIZE ||
          s_rx.hdr.nbytes > USB_STREAM_MSG_MAX - USB_STREAM_HDR_SIZE)
      {
        USB_App_RecordBad(USB_BAD_HEADER);
        memmove(dst, dst + 1u, USB_STREAM_HDR_SIZE - 1u);
        s_rx.header_got = USB_STREAM_HDR_SIZE - 1u;
        continue;
      }
      expected = s_last_pack_sequence == 0xFFFEu
                     ? 0u
                     : (uint16_t)(s_last_pack_sequence + 1u);
      s_transport.pack_sequence = s_rx.hdr.pad;
      s_transport.active = s_rx.hdr.pad == expected ? 1u : 2u;
      if (s_transport.active == 2u &&
          s_rx.hdr.pad != s_last_pack_sequence)
        USB_App_RecordBad(USB_BAD_SEQUENCE);
      s_rx.payload_left = s_rx.hdr.nbytes;
      if (s_transport.active == 2u)
      {
        s_rx.payload_left += USB_STREAM_CRC_BYTES;
        s_rx.state = USB_RX_DISCARD;
        s_rx.header_got = 0u;
        continue;
      }
      s_rx.crc = USB_App_Crc32Update(0xFFFFFFFFu, dst,
                                     USB_STREAM_HDR_SIZE);
      s_rx_bytes += USB_STREAM_HDR_SIZE;
      s_rx.state = USB_RX_META;
      continue;
    }
    if (s_rx.state == USB_RX_META)
    {
      uint8_t *dst = (uint8_t *)(void *)&s_rx.meta;
      n = USB_STREAM_BODY_META_SIZE - s_rx.meta_got;
      if (n > count)
        n = count;
      memcpy(dst + s_rx.meta_got, src, n);
      s_rx.crc = USB_App_Crc32Update(s_rx.crc, src, n);
      s_rx.meta_got += n;
      s_rx.payload_left -= n;
      src += n;
      count -= n;
      s_rx_bytes += n;
      if (s_rx.meta_got < USB_STREAM_BODY_META_SIZE)
        continue;
      s_rx.sample_bytes_left = (uint32_t)s_rx.meta.nsamp * 2u;
      if (s_rx.write_count >= 8u || s_rx.meta.voice >= 8u ||
          (s_rx.voice_mask & (uint8_t)(1u << s_rx.meta.voice)) != 0u ||
          (s_rx.meta.flags & (uint8_t)~USB_STREAM_BODY_FLAG_SOF) != 0u ||
          s_rx.meta.nsamp == 0u || s_rx.meta.nsamp > USB_STREAM_NSAMP_MAX ||
          s_rx.sample_bytes_left > s_rx.payload_left)
      {
        USB_App_FailFrame(USB_BAD_FRAME);
        return initial;
      }
      {
        int begin_result = USB_App_BeginMetaWrite();
        if (begin_result < 0)
          return initial;
        if (begin_result == 0)
          return initial - count;
      }
      continue;
    }
    if (s_rx.state == USB_RX_SAMPLES)
    {
      StreamRing_Write_t *write = &s_rx.writes[s_rx.current_write];
      uint32_t span_samples = 0u;
      int16_t *span = NULL;
      if ((s_rx.stale_write_mask &
           (uint8_t)(1u << s_rx.current_write)) != 0u)
      {
        n = s_rx.sample_bytes_left;
        if (n > count)
          n = count;
        n &= ~1u;
      }
      else
      {
        span = StreamRing_WriteSpan(write, &span_samples);
        if (span == NULL || span_samples == 0u)
        {
          USB_App_FailFrame(USB_BAD_FRAME);
          return initial;
        }
        n = span_samples * 2u;
        if (n > s_rx.sample_bytes_left)
          n = s_rx.sample_bytes_left;
        if (n > count)
          n = count;
        n &= ~1u;
      }
      if (n == 0u)
        return initial;
      if (span != NULL)
      {
        memcpy(span, src, n);
        if (StreamRing_WriteAdvance(write, n / 2u) != 0)
        {
          USB_App_FailFrame(USB_BAD_FRAME);
          return initial;
        }
      }
      s_rx.crc = USB_App_Crc32Update(s_rx.crc, src, n);
      s_rx.sample_bytes_left -= n;
      s_rx.payload_left -= n;
      src += n;
      count -= n;
      s_rx_bytes += n;
      if (s_rx.sample_bytes_left != 0u)
        continue;
      if (s_rx.payload_left == 0u)
      {
        s_rx.state = USB_RX_CRC;
        s_rx.crc_got = 0u;
        continue;
      }
      if (s_rx.payload_left < USB_STREAM_BODY_META_SIZE)
      {
        USB_App_FailFrame(USB_BAD_FRAME);
        return initial;
      }
      memset(&s_rx.meta, 0, sizeof s_rx.meta);
      s_rx.meta_got = 0u;
      s_rx.state = USB_RX_META;
      continue;
    }
    if (s_rx.state == USB_RX_CRC)
    {
      uint32_t expected_crc;
      n = USB_STREAM_CRC_BYTES - s_rx.crc_got;
      if (n > count)
        n = count;
      memcpy(s_rx.crc_bytes + s_rx.crc_got, src, n);
      s_rx.crc_got += (uint8_t)n;
      src += n;
      count -= n;
      if (s_rx.crc_got < USB_STREAM_CRC_BYTES)
        continue;
      memcpy(&expected_crc, s_rx.crc_bytes, sizeof expected_crc);
      if ((s_rx.crc ^ 0xFFFFFFFFu) != expected_crc)
      {
        USB_App_FailFrame(USB_BAD_CRC);
        continue;
      }
      (void)USB_App_FinishFrame();
    }
  }
  return initial;
}

static void USB_App_DrainUac(void)
{
  uint32_t budget = USB_STREAM_UAC_PACKET_BYTES;
  if (!tud_audio_mounted())
  {
    s_uac_pending_offset = 0u;
    s_uac_pending_bytes = 0u;
    USB_App_AbortTransport();
    return;
  }
  /* Bound each call to one physical USB millisecond. The main loop invokes
   * this on both sides of ChannelConsole_Poll(), so sustained UAC input
   * cannot monopolize the loop and starve the vq request that authorized it.
   * The 32 ms TinyUSB FIFO absorbs short console/TX service intervals. */
  if (s_uac_pending_bytes != 0u)
  {
    uint32_t used = USB_App_FeedPack(
        s_uac_packet + s_uac_pending_offset, s_uac_pending_bytes);
    s_uac_pending_offset += (uint16_t)used;
    s_uac_pending_bytes -= (uint16_t)used;
    if (s_uac_pending_bytes != 0u)
      return;
    s_uac_pending_offset = 0u;
  }
  while (budget >= USB_STREAM_UAC_AUDIO_FRAME_BYTES &&
         (uint32_t)tud_audio_available() >= USB_STREAM_UAC_AUDIO_FRAME_BYTES)
  {
    uint32_t available = tud_audio_available();
    uint32_t want = available;
    uint16_t n;
    if (want > sizeof s_uac_packet)
      want = sizeof s_uac_packet;
    if (want > budget)
      want = budget;
    want -= want % USB_STREAM_UAC_AUDIO_FRAME_BYTES;
    n = tud_audio_read(s_uac_packet, (uint16_t)want);
    if (n == 0u || (n % USB_STREAM_UAC_AUDIO_FRAME_BYTES) != 0u)
    {
      USB_App_RecordBad(USB_BAD_UAC);
      USB_App_AbortTransport();
      break;
    }
    budget -= n;
    s_uac_audio_frames += n / USB_STREAM_UAC_AUDIO_FRAME_BYTES;
    while (s_uac_audio_frames >= USB_STREAM_UAC_FRAMES_PER_MS)
    {
      s_uac_audio_frames -= USB_STREAM_UAC_FRAMES_PER_MS;
      s_uac_windows++;
    }
    {
      uint32_t used = USB_App_FeedPack(s_uac_packet, n);
      if (used < n)
      {
        s_uac_pending_offset = (uint16_t)used;
        s_uac_pending_bytes = (uint16_t)(n - used);
        return;
      }
    }
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

uint16_t USB_App_LastPackSequence(void) { return s_last_pack_sequence; }
uint32_t USB_App_RxMsgCount(void) { return s_rx_msg; }
uint32_t USB_App_RxByteCount(void) { return s_rx_bytes; }
uint32_t USB_App_UacWindowCount(void) { return s_uac_windows; }
uint32_t USB_App_BadCount(void) { return s_bad; }
uint32_t USB_App_BadReasonCount(uint8_t reason)
{
  return reason < USB_BAD_REASON_COUNT ? s_bad_reason[reason] : 0u;
}

void USB_App_StatsClear(void)
{
  s_rx_msg = 0u;
  s_rx_bytes = 0u;
  s_uac_windows = 0u;
  s_bad = 0u;
  memset(s_bad_reason, 0, sizeof s_bad_reason);
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
  s_uac_pending_offset = 0u;
  s_uac_pending_bytes = 0u;
  USB_App_AbortTransport();
  s_uac_audio_frames = 0u;
  if ((uint8_t)tu_le16toh(request->wValue) != 0u)
  {
    s_last_pack_sequence = 0xFFFFu;
    Audio_Bridge_Start();
  }
  return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const *request)
{
  (void)rhport; (void)request;
  s_uac_pending_offset = 0u;
  s_uac_pending_bytes = 0u;
  USB_App_AbortTransport();
  s_uac_audio_frames = 0u;
  Audio_Bridge_StreamStop();
  return true;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t nbytes,
                                    uint8_t func_id, uint8_t ep_out,
                                    uint8_t alt)
{
  (void)rhport; (void)nbytes; (void)func_id; (void)ep_out; (void)alt;
  return true;
}
