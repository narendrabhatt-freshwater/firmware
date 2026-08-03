/**
 ******************************************************************************
 * @file    audio_bridge.h
 * @brief   USB audio -> I2S bridge for the CS4304 4-channel DAC.
 *
 * Relocated from USB_DEVICE/App/usbd_audio_if.c when the card moved from
 * ST's USB Device Library to TinyUSB.  Everything below the USB boundary
 * (I2S DMA ring buffers, tone/DC generators, CS4304 handling) is
 * unchanged; only the stack that feeds Audio_Bridge_WriteUSB() differs.
 ******************************************************************************
 */

#ifndef __AUDIO_BRIDGE_H__
#define __AUDIO_BRIDGE_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  /* ---------------- USB stack facing API (called by USB_APP) ---------------- */

  /** Clear the I2S buffers and start I2S1 (+I2S2) DMA.  Safe to call from
   * USB interrupt context: it busy-waits rather than using HAL_Delay. */
  void Audio_Bridge_Start(void);

  /** Stop I2S DMA and reset the stream state. */
  void Audio_Bridge_Stop(void);

  /** Host closed the streaming interface: silence the buffer and force the
   * write pointer to re-acquire its lead on the next stream. */
  void Audio_Bridge_StreamStop(void);

  /** Feed one USB isochronous OUT packet (mono 32-bit PCM) into the I2S1
   * ring buffer.  `size` is in bytes. */
  void Audio_Bridge_WriteUSB(const uint8_t *pbuf, uint32_t size);

  /** UAC volume/mute forwarded to the DAC. */
  void Audio_Bridge_SetVolume(uint8_t vol);
  void Audio_Bridge_SetMute(uint8_t mute);

  /** Mute the USB playback channel (CH1).  Volume is applied by Windows in
   * software (mute-only feature unit); this only gates CH1 to silence.
   * Never affects the CH2..CH4 tone/DC generators or the DAC 'gain'
   * command. */
  void Audio_SetUSBMute(uint8_t mute);

  /* ---------------- I2S DMA callbacks (internal, kept public) -------------- */

  void TransferComplete_CallBack_HS(void);
  void HalfTransfer_CallBack_HS(void);

  /* ---------------- Console / application API ------------------------------ */

  /** Start I2S playback outside of USB streaming (test tones). */
  void Audio_StartPlayback(void);

  void Audio_SetToneFreq(uint8_t channel, uint32_t freq_hz);
  uint32_t Audio_GetToneFreq(uint8_t channel);

  /** TIM7 fallback pump for the I2S2 slave (UDR guard). */
  void Audio_I2S2_Pump(void);

  typedef enum
  {
    AUDIO_MODE_TONE = 0, /**< Sine wave (default) */
    AUDIO_MODE_DC = 1,   /**< Constant DC level   */
  } Audio_ChannelMode_t;

  void Audio_SetChannelMode(uint8_t channel, Audio_ChannelMode_t mode);
  Audio_ChannelMode_t Audio_GetChannelMode(uint8_t channel);

  /* ---------------- CH1 source select (USB vs N0–NF note bank) ------------- */

  typedef enum
  {
    AUDIO_CH1_SRC_USB = 0,       /**< CH1 carries USB PCM (default) */
    AUDIO_CH1_SRC_TEST_TONE = 1, /**< CH1 carries the N0–NF note bank mix;
                                      USB samples are ignored while active */
  } Audio_CH1_Source_t;

  /** Current CH1 source: note bank when NoteBank_AnyActive(), else USB.
   * Frequency control lives in note_bank.h (NoteBank_SetFreq). */
  Audio_CH1_Source_t Audio_GetCh1Source(void);

  void Audio_SetDCLevel(uint8_t channel, int8_t percent);
  int8_t Audio_GetDCLevel(uint8_t channel);

  void Audio_SetDCLimit(uint8_t pct_fs);
  uint8_t Audio_GetDCLimit(void);

  void Audio_SetDCInvert(uint8_t channel, uint8_t invert);
  uint8_t Audio_GetDCInvert(uint8_t channel);

  void Audio_SetDCZero(uint8_t channel, int8_t zero_pct);
  int8_t Audio_GetDCZero(uint8_t channel);

  void Audio_SetDCTrim(uint8_t channel, int16_t trim_x100);
  int16_t Audio_GetDCTrim(uint8_t channel);

  /* ---------------- CPU load probe (LED_Y / PB9) --------------------------- */

  typedef enum
  {
    AUDIO_CPULOAD_OFF = 0,   /**< Normal path; LED_Y free for chaser */
    AUDIO_CPULOAD_DMA = 1,   /**< Busy=low around NoteBank DMA half fill */
    AUDIO_CPULOAD_QUEUE = 2, /**< Soft queue (256) filled in main, drained by DMA */
  } Audio_CpuLoadMode_t;

  void Audio_CpuLoad_SetMode(Audio_CpuLoadMode_t mode);
  Audio_CpuLoadMode_t Audio_CpuLoad_GetMode(void);

  /** Non-zero while DMA or queue probe owns LED_Y (pause LED chaser). */
  uint8_t Audio_CpuLoad_IsActive(void);

  /** Queue-mode producer: call from main loop. No-op unless QUEUE mode. */
  void Audio_CpuLoad_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_BRIDGE_H__ */
