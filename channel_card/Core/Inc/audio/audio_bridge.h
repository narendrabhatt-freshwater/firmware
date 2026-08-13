/**
 ******************************************************************************
 * @file    audio_bridge.h
 * @brief   USB audio / note-bank → I2S bridge for the CS4304 4-channel DAC.
 *
 * Owns I2S DMA ring buffers, USB packet ingest, CH1 note-bank refill,
 * and the TIM7 I2S2 underrun pump. Tone/DC generators and the CPU-load
 * probe live in audio_tone_dc.h / audio_cpuload.h (re-exported here so
 * existing callers that include only audio_bridge.h keep working).
 * UAC dry multi-channel PCM feeds Audio_Bridge_WriteUSB() into stream
 * rings (not the DAC). CH1 is always the SAMPLE note-bank mix.
 ******************************************************************************
 */

#ifndef __AUDIO_BRIDGE_H__
#define __AUDIO_BRIDGE_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "cs4304.h"
#include "audio_cpuload.h"
#include "audio_tone_dc.h"

  /* ---------------- DAC handle (bound from main) --------------------------- */

  /**
   * @brief Bind the CS4304 handle used for UAC volume/mute.
   * @param h DAC handle owned by main (non-NULL before Start / SetVolume).
   */
  void Audio_Bridge_SetDacHandle(CS4304_HandleTypeDef *h);

  /* ---------------- USB stack facing API (called by USB_APP) ---------------- */

  /**
   * @brief Clear I2S buffers and start I2S1 (+I2S2) DMA.
   * @note Safe from USB IRQ context: busy-waits instead of HAL_Delay.
   */
  void Audio_Bridge_Start(void);

  /** @brief Stop I2S DMA and reset stream state. */
  void Audio_Bridge_Stop(void);

  /**
   * @brief Host closed the streaming interface.
   * @note Silences the buffer and forces the write pointer to re-acquire
   *       its lead on the next stream.
   */
  void Audio_Bridge_StreamStop(void);

  /**
   * @brief Feed one USB isochronous OUT packet (8ch int16 interleaved) into
   *        per-voice dry stream rings for SAMPLE sustain.
   * @param pbuf Packet bytes (may be NULL only when size is 0).
   * @param size Length in bytes.
   */
  void Audio_Bridge_WriteUSB(const uint8_t *pbuf, uint32_t size);

  /**
   * @brief Apply UAC master volume to the bound DAC handle.
   * @param vol Attenuation code (0.5 dB steps; see CS4304_SetVolume).
   */
  void Audio_Bridge_SetVolume(uint8_t vol);

  /**
   * @brief Apply UAC mute to the bound DAC handle.
   * @param mute Non-zero to mute.
   */
  void Audio_Bridge_SetMute(uint8_t mute);

  /**
   * @brief Soft-mute USB playback on CH1 only (host volume is software-side).
   * @param mute Non-zero gates CH1 to silence.
   * @note Never affects CH2..CH4 generators or the console `gain` command.
   */
  void Audio_SetUSBMute(uint8_t mute);

  /* ---------------- I2S DMA callbacks (internal, kept public) -------------- */

  /** @brief I2S1 DMA full-transfer callback. */
  void TransferComplete_CallBack_HS(void);

  /** @brief I2S1 DMA half-transfer callback. */
  void HalfTransfer_CallBack_HS(void);

  /* ---------------- Console / application API ------------------------------ */

  /**
   * @brief Start I2S playback outside of USB streaming (test tones / bank).
   */
  void Audio_StartPlayback(void);

  /**
   * @brief TIM7 fallback pump for the I2S2 slave (underrun guard).
   * @note Called from TIM7 IRQ; must complete in microseconds.
   */
  void Audio_I2S2_Pump(void);

  /* ---------------- CH1 source (SAMPLE note bank) --------------------------- */

  typedef enum
  {
    AUDIO_CH1_SRC_USB = 0,       /**< Unused in SAMPLE mode (dry UAC → rings) */
    AUDIO_CH1_SRC_TEST_TONE = 1, /**< CH1 carries SAMPLE note-bank mix */
  } Audio_CH1_Source_t;

  /** Always AUDIO_CH1_SRC_TEST_TONE in SAMPLE mode. */
  Audio_CH1_Source_t Audio_GetCh1Source(void);

  /**
   * @brief I2S1 half-buffer refill hook (no-op: fill runs in the DMA ISR).
   */
  void Audio_I2S1_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_BRIDGE_H__ */
