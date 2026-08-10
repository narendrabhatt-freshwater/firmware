/* USB application layer — Effect Card
 *
 * UAC2 mono microphone (32-bit / 96 kHz) + CDC-ACM console, on OTG_FS.
 *
 * Audio data path: the SAI DMA capture in main.c fills usb_audio_frame[]
 * with ADC samples of the channel selected by usb_adc_ch
 * (`u <1..8>` console command).
 */

#include "usb_app.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

volatile uint8_t usb_adc_ch = 1; /* selected ADC channel, 1..8 */

/* UAC2 control state */
static bool mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static uint16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX + 1];
static uint32_t sampFreq = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
static uint8_t clkValid = 1;
static audio_control_range_4_n_t(1) sampleFreqRng;

void USB_Audio_Write(const uint8_t *data, uint16_t len) {
  if (!tud_audio_mounted())
    return;
  tud_audio_write(data, len);
}

//--------------------------------------------------------------------+
// Init / task
//--------------------------------------------------------------------+

void USB_App_Init(void) {
  sampleFreqRng.wNumSubRanges = 1;
  sampleFreqRng.subrange[0].bMin = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bMax = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
  sampleFreqRng.subrange[0].bRes = 0;

  tud_init(BOARD_TUD_RHPORT);
}

//--------------------------------------------------------------------+
// CDC console: line-buffered bridge into Console_Exec (effect_console.c)
//--------------------------------------------------------------------+

void USB_CDC_WriteStr(const char *s) {
  if (!tud_cdc_connected())
    return;
  uint32_t len = (uint32_t)strlen(s);
  uint32_t sent = 0;
  while (sent < len) {
    uint32_t n = tud_cdc_write(s + sent, len - sent);
    sent += n;
    tud_cdc_write_flush();
    if (n == 0) {
      /* host not draining — service the stack once, then give up
       * rather than blocking the main loop forever */
      tud_task();
      if (!tud_cdc_connected())
        return;
    }
  }
}

static void CDC_Console_Poll(void) {
  static char line[64];
  static uint8_t len = 0;

  while (tud_cdc_available()) {
    char c;
    if (tud_cdc_read(&c, 1) == 0)
      break;

    if (c == '\r' || c == '\n') {
      USB_CDC_WriteStr("\r\n");
      if (len > 0) {
        line[len] = '\0';
        len = 0;
        Console_ExecFromUSB(line);
      }
    } else if (c == 0x08 || c == 0x7F) { /* backspace */
      if (len > 0) {
        len--;
        USB_CDC_WriteStr("\b \b");
      }
    } else if (len < sizeof(line) - 1 && c >= 0x20 && c < 0x7F) {
      /* lowercase to match the RS485 console behaviour */
      if (c >= 'A' && c <= 'Z')
        c = (char)(c + 32);
      line[len++] = c;
      char e[2] = {c, '\0'};
      USB_CDC_WriteStr(e); /* echo */
    }
  }
}

void USB_App_Task(void) {
  tud_task();
  CDC_Console_Poll();
}

//--------------------------------------------------------------------+
// UAC2 audio callbacks
//--------------------------------------------------------------------+

/* Entity IDs fixed by TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR:
 *   1 = input terminal, 2 = feature unit, 3 = output terminal, 4 = clock */

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request,
                             uint8_t *pBuff) {
  (void)rhport;
  (void)p_request;
  (void)pBuff;
  return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request,
                              uint8_t *pBuff) {
  (void)rhport;
  (void)p_request;
  (void)pBuff;
  return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request,
                                 uint8_t *pBuff) {
  (void)rhport;
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  TU_VERIFY(p_request->bRequest == AUDIO_CS_REQ_CUR);

  if (entityID == 2) { /* feature unit */
    switch (ctrlSel) {
    case AUDIO_FU_CTRL_MUTE:
      TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_1_t));
      mute[channelNum] = ((audio_control_cur_1_t *)pBuff)->bCur;
      return true;
    case AUDIO_FU_CTRL_VOLUME:
      TU_VERIFY(p_request->wLength == sizeof(audio_control_cur_2_t));
      volume[channelNum] = (uint16_t)((audio_control_cur_2_t *)pBuff)->bCur;
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *p_request) {
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport,
                              tusb_control_request_t const *p_request) {
  (void)rhport;
  (void)p_request;
  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *p_request) {
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  if (entityID == 1) { /* input terminal */
    if (ctrlSel == AUDIO_TE_CTRL_CONNECTOR) {
      audio_desc_channel_cluster_t ret;
      ret.bNrChannels = 1;
      ret.bmChannelConfig = (audio_channel_config_t)0;
      ret.iChannelNames = 0;
      return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                        (void *)&ret,
                                                        sizeof(ret));
    }
    return false;
  }

  if (entityID == 2) { /* feature unit */
    switch (ctrlSel) {
    case AUDIO_FU_CTRL_MUTE:
      return tud_control_xfer(rhport, p_request, &mute[channelNum], 1);
    case AUDIO_FU_CTRL_VOLUME:
      switch (p_request->bRequest) {
      case AUDIO_CS_REQ_CUR:
        return tud_control_xfer(rhport, p_request, &volume[channelNum],
                                sizeof(volume[channelNum]));
      case AUDIO_CS_REQ_RANGE: {
        audio_control_range_2_n_t(1) ret;
        ret.wNumSubRanges = 1;
        ret.subrange[0].bMin = -90;
        ret.subrange[0].bMax = 30;
        ret.subrange[0].bRes = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                          (void *)&ret,
                                                          sizeof(ret));
      }
      default:
        return false;
      }
    default:
      return false;
    }
  }

  if (entityID == 4) { /* clock source */
    switch (ctrlSel) {
    case AUDIO_CS_CTRL_SAM_FREQ:
      switch (p_request->bRequest) {
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

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in,
                                   uint8_t cur_alt_setting) {
  (void)rhport;
  (void)itf;
  (void)ep_in;
  (void)cur_alt_setting;

  /* Nothing to do: the SAI DMA callbacks (main.c) push captured ADC
   * samples into the FIFO via USB_Audio_Write(); the class driver drains
   * it on every IN token.  If the FIFO is momentarily empty (ADCs not
   * initialised yet), short/zero packets go out — harmless silence. */
  return true;
}

bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied,
                                    uint8_t itf, uint8_t ep_in,
                                    uint8_t cur_alt_setting) {
  (void)rhport;
  (void)n_bytes_copied;
  (void)itf;
  (void)ep_in;
  (void)cur_alt_setting;
  return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const *p_request) {
  (void)rhport;
  (void)p_request;
  return true;
}
