/* USB application layer — Channel Card
 *
 * UAC2 mono speaker (32-bit / 96 kHz) + CDC-ACM console on USB1_OTG_HS.
 *
 * Audio path: host -> iso OUT -> TinyUSB FIFO -> Audio_Bridge_WriteUSB()
 * -> I2S1 ring buffer -> CS4304 DAC channel 1.  Everything downstream of
 * Audio_Bridge_WriteUSB() is the original, tuned bridge code, unchanged
 * from when ST's stack drove it.
 */

#include "usb_app.h"
#include "audio_bridge.h"
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
      /* host not draining — service the stack once, then give up rather
       * than blocking the main loop (and the audio path) forever */
      tud_task();
      if (!tud_cdc_connected())
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

void USB_App_Task(void)
{
  tud_task();
  CDC_Console_Poll();
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
      /* master(0) or per-channel(1): silence CH1 if either is muted */
      Audio_SetUSBMute(mute[0] || mute[1]);
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
    /* Nominal feedback: 96 samples per 1 ms frame, 16.16 format (TinyUSB
     * converts to 10.14 for full speed because FEEDBACK_FORMAT_CORRECTION
     * is enabled).  See tud_audio_feedback_params_cb() below. */
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

/** Feedback strategy: manual/fixed.
 *
 * The driver's FIFO_COUNT method regulates TinyUSB's own FIFO to half
 * full, which assumes the FIFO is the elastic buffer.  Here the elastic
 * buffer is the I2S ring (with its NDTR-derived write lead and drift
 * re-centring) and the FIFO is drained completely every packet, so
 * FIFO_COUNT would fight that loop.  Report the nominal rate instead and
 * let the proven ring-buffer guard absorb drift. */
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
  (void)func_id;
  (void)alt_itf;
  feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
  feedback_param->sample_freq = CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE;
}

/** One isochronous OUT packet has landed in the class FIFO: drain it
 * straight into the I2S ring buffer. */
bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received,
                                    uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting)
{
  (void)rhport;
  (void)func_id;
  (void)ep_out;
  (void)cur_alt_setting;

  static uint8_t pkt[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX];
  if (n_bytes_received > sizeof(pkt))
    n_bytes_received = sizeof(pkt);

  uint16_t n = tud_audio_read(pkt, n_bytes_received);
  if (n)
  {
    Audio_Bridge_WriteUSB(pkt, n);
  }
  return true;
}
