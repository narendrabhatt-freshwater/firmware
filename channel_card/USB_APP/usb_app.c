/* USB application layer — Channel Card
 *
 * UAC2 10ch int16 @ 48 kHz dry → stream rings + CDC console / attack load.
 */

#include "usb_app.h"
#include "audio_bridge.h"
#include "attack_upload.h"
#include "channel_console.h"
#include "main.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

/* UAC2 control state.  The device advertises a MUTE-only feature unit, so
 * Windows applies volume itself by scaling the PCM before sending it (see
 * usb_descriptors.c for why mute-only rather than a hardware volume
 * control).  We only track/forward mute. */
static bool mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];
static uint32_t sampFreq = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
static uint8_t clkValid = 1;
static audio_control_range_4_n_t(1) sampleFreqRng;

static void USB_App_DrainUac(void);

//--------------------------------------------------------------------+
// Init / task
//--------------------------------------------------------------------+

/** Bring up the USB peripheral clocks, PHY power and NVIC.
 * Mirrors what ST's HAL_PCD_MspInit() used to do (PLL3 -> 48 MHz USB
 * kernel clock, USB voltage detector, peripheral clock, OTG_HS IRQ);
 * TinyUSB itself only touches the core registers. */
static void USB_LowLevel_Init(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInitStruct.PLL3.PLL3M = 4;
  PeriphClkInitStruct.PLL3.PLL3N = 125;
  PeriphClkInitStruct.PLL3.PLL3P = 16;
  PeriphClkInitStruct.PLL3.PLL3Q = 16; /* 768 MHz / 16 = 48 MHz exactly */
  PeriphClkInitStruct.PLL3.PLL3R = 8;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 0;
  PeriphClkInitStruct.UsbClockSelection = RCC_USBCLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /* Transceiver supply — without this the D+ pull-up never appears and
   * the host sees nothing on the bus. */
  HAL_PWREx_EnableUSBVoltageDetector();

  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

  /* ISO OUT has no retry — USB must preempt I2S1 DMA fill (prio 2). */
  HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}

void USB_App_Init(void)
{
  sampleFreqRng.wNumSubRanges = 1;
  sampleFreqRng.subrange[0].bMin = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bMax = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bRes = 0;

  USB_LowLevel_Init();
  tud_init(BOARD_TUD_RHPORT);
}

//--------------------------------------------------------------------+
// CDC console: line-buffered bridge into Console_ExecFromUSB
//--------------------------------------------------------------------+

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
      /* TX buffer full. Do not spin in tud_task: that races the USB ISR
       * and skips ISO OUT re-arm (INCOMPISOOUT). */
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
    /* Binary attack upload: consume raw bytes, no echo / line edit. */
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
    { /* backspace */
      if (len > 0)
      {
        len--;
        USB_CDC_WriteStr("\b \b");
      }
    }
    else if (len < sizeof(line) - 1 && c >= 0x20 && c < 0x7F)
    {
      /* lowercase to match the RS485 console behaviour */
      if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
      line[len++] = c;
      char e[2] = {c, '\0'};
      USB_CDC_WriteStr(e); /* echo */
    }
  }
}

/* ISO OUT is armed from tud_task, not the DCD ISR. Run tud_task only in
 * the USB ISR. Main must not call it: OS_NONE is not re-entrant, and a
 * skip here misses the next SOF (INCOMPISOOUT, then a draining ring). */
static volatile uint8_t s_tud_from_isr;
static volatile uint32_t s_iso_drop;
static volatile uint32_t s_iso_incomp;
static volatile uint32_t s_n47;
static volatile uint32_t s_n48;
static volatile uint32_t s_n49;
static volatile uint32_t s_nxx;

/* TinyUSB never unmasks ISOODRP / incomplete ISO OUT, so a dropped
 * packet never reaches the body FIFO and never increments `usb drop`. */
static void USB_App_PollIsoHw(void)
{
  uint32_t sts = USB_OTG_HS->GINTSTS;

  if ((sts & USB_OTG_GINTSTS_ISOODRP) != 0u)
  {
    s_iso_drop++;
    USB_OTG_HS->GINTSTS = USB_OTG_GINTSTS_ISOODRP;
  }
  if ((sts & USB_OTG_GINTSTS_PXFR_INCOMPISOOUT) != 0u)
  {
    s_iso_incomp++;
    USB_OTG_HS->GINTSTS = USB_OTG_GINTSTS_PXFR_INCOMPISOOUT;
  }
}

void USB_App_TaskFromIsr(void)
{
  /* Re-arm ISO OUT. Never skip: a busy-lock with main's tud_task missed
   * SOFs. Do not WriteUSB here — demux in this IRQ ran past the next SOF. */
  if (s_tud_from_isr != 0u)
  {
    return;
  }
  s_tud_from_isr = 1u;
  tud_task();
  s_tud_from_isr = 0u;
}

static uint8_t s_uac_pkt[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX];

static void USB_App_CountPktSize(uint16_t n_bytes)
{
  uint32_t frame_bytes = (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX *
                         (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX;
  uint32_t frames;

  if (frame_bytes == 0u || (n_bytes % frame_bytes) != 0u)
  {
    s_nxx++;
    return;
  }
  frames = (uint32_t)n_bytes / frame_bytes;
  if (frames == 47u)
  {
    s_n47++;
  }
  else if (frames == 48u)
  {
    s_n48++;
  }
  else if (frames == 49u)
  {
    s_n49++;
  }
  else
  {
    s_nxx++;
  }
}

/* Copy TinyUSB's SW FIFO into the body rings. Drain it dry: a full
 * software FIFO skips the next EP OUT xfer (INCOMPISOOUT). */
static void USB_App_DrainUac(void)
{
  uint32_t frame_bytes = (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX *
                         (uint32_t)CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX;

  if (!tud_audio_mounted())
  {
    return;
  }
  for (;;)
  {
    uint16_t n;

    if ((uint32_t)tud_audio_available() < frame_bytes)
    {
      break;
    }
    n = tud_audio_read(s_uac_pkt, (uint16_t)sizeof s_uac_pkt);
    if (n == 0u)
    {
      break;
    }
    Audio_Bridge_WriteUSB(s_uac_pkt, n);
  }
}

void USB_App_Task(void)
{
  static uint32_t fb_tick;

  USB_App_PollIsoHw();
  USB_App_DrainUac();
  CDC_Console_Poll();
  {
    uint32_t now = HAL_GetTick();
    if ((now - fb_tick) >= 8u)
    {
      fb_tick = now;
      if (tud_audio_mounted())
      {
        (void)tud_audio_fb_set(Audio_Bridge_FeedbackU16());
      }
    }
  }
}

uint32_t USB_App_IsoDropCount(void)
{
  return s_iso_drop;
}

uint32_t USB_App_IsoIncompCount(void)
{
  return s_iso_incomp;
}

void USB_App_PktSizeCounts(uint32_t *n47, uint32_t *n48, uint32_t *n49,
                           uint32_t *nxx)
{
  if (n47 != NULL)
  {
    *n47 = s_n47;
  }
  if (n48 != NULL)
  {
    *n48 = s_n48;
  }
  if (n49 != NULL)
  {
    *n49 = s_n49;
  }
  if (nxx != NULL)
  {
    *nxx = s_nxx;
  }
}

void USB_App_StatsClear(void)
{
  s_iso_drop = 0u;
  s_iso_incomp = 0u;
  s_n47 = 0u;
  s_n48 = 0u;
  s_n49 = 0u;
  s_nxx = 0u;
}

//--------------------------------------------------------------------+
// UAC2 audio callbacks
//--------------------------------------------------------------------+

/* Entity IDs fixed by TUD_AUDIO_SPEAKER_MONO_FB_DESCRIPTOR:
 *   1 = input terminal, 2 = feature unit, 3 = output terminal, 4 = clock */

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *p_request,
                             uint8_t *pBuff)
{
  (void)rhport;
  (void)p_request;
  (void)pBuff;
  return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request,
                              uint8_t *pBuff)
{
  (void)rhport;
  (void)p_request;
  (void)pBuff;
  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request,
                                 uint8_t *pBuff)
{
  (void)rhport;
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  if (entityID == 2)
  { /* feature unit */
    switch (ctrlSel)
    {
    case AUDIO_FU_CTRL_MUTE:
      TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));
      mute[channelNum] = ((audio_control_cur_1_t *)pBuff)->bCur;
      /* UAC is a tagged body pipe, not analog PCM. macOS mutes unused
       * channels 2–8; OR-ing those would idle the rings and kill sustain. */
      return true;

    default:
      return false;
    }
  }
  return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *p_request)
{
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request)
{
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request)
{
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  if (entityID == 1)
  { /* input terminal */
    if (ctrlSel == AUDIO_TE_CTRL_CONNECTOR)
    {
      audio_desc_channel_cluster_t ret;
      ret.bNrChannels = CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX;
      ret.bmChannelConfig = (audio_channel_config_t)0;
      ret.iChannelNames = 0;
      return tud_audio_buffer_and_schedule_control_xfer(
          rhport, p_request, (void *)&ret, sizeof(ret));
    }
    return false;
  }

  if (entityID == 2)
  { /* feature unit */
    switch (ctrlSel)
    {
    case AUDIO_FU_CTRL_MUTE:
      return tud_control_xfer(rhport, p_request, &mute[channelNum], 1);

    default:
      return false;
    }
  }

  if (entityID == 4)
  { /* clock source */
    switch (ctrlSel)
    {
    case AUDIO_CS_CTRL_SAM_FREQ:
      switch (p_request->bRequest)
      {
      case AUDIO_CS_REQ_CUR:
        return tud_control_xfer(rhport, p_request, &sampFreq, sizeof(sampFreq));
      case AUDIO_CS_REQ_RANGE:
        return tud_control_xfer(rhport, p_request, &sampleFreqRng,
                                sizeof(sampleFreqRng));
      default:
        return false;
      }
    case AUDIO_CS_CTRL_CLK_VALID:
      return tud_control_xfer(rhport, p_request, &clkValid, sizeof(clkValid));
    default:
      return false;
    }
  }

  return false;
}

/** Host opened the streaming interface (alt != 0) -> start I2S. */
bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *p_request)
{
  (void)rhport;
  uint8_t const alt = (uint8_t)tu_le16toh(p_request->wValue);

  if (alt != 0)
  {
    Audio_Bridge_Start();
    /* Nominal feedback: 48 samples per 1 ms frame @ 48 kHz, 16.16. */
    tud_audio_n_fb_set(0, (CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE / 1000) << 16);
  }
  return true;
}

/** Host closed the streaming interface -> silence and re-arm. */
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const *p_request)
{
  (void)rhport;
  (void)p_request;
  Audio_Bridge_StreamStop();
  return true;
}

/** Feedback strategy: manual 16.16 from body-FIFO fill (not TinyUSB's
 * ISO FIFO). A fixed 48.000 lets the Mac clock walk vs I2S until the
 * ring overflows and a packet is dropped. */
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
  (void)func_id;
  (void)alt_itf;
  feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
  feedback_param->sample_freq = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
}

/** Iso OUT complete: count size only. Demux runs in USB_App_DrainUac(). */
bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received,
                                    uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting)
{
  (void)rhport;
  (void)func_id;
  (void)ep_out;
  (void)cur_alt_setting;

  USB_App_CountPktSize(n_bytes_received);
  return true;
}
