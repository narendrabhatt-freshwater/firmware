/**
 ******************************************************************************
 * @file    audio_bridge.h
 * @brief   USB audio / note-bank → I2S bridge for the CS4304 4-channel DAC.
 *
 * Owns I2S DMA ring buffers, USB BODY ingest, CH1 note-bank refill,
 * and the TIM7 I2S2 underrun pump. Tone/DC generators live in
 * audio_tone_dc.h (re-exported here for existing callers).
 * Vendor bulk BODY feeds StreamRing_WriteVoice (not the DAC). CH1 is
 * always the SAMPLE note-bank mix.
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
#include "audio_tone_dc.h"

  /* ---------------- DAC handle (bound from main) --------------------------- */

  /**
   * @brief Bind the CS4304 handle used for mute/volume.
   * @param h DAC handle owned by main (non-NULL before Start / SetVolume).
   */
  void Audio_Bridge_SetDacHandle(CS4304_HandleTypeDef *h);

  /* ---------------- USB stack facing API (called by USB_APP) ---------------- */

  /**
   * @brief Clear I2S buffers and start I2S1 (+I2S2) DMA.
   * @note Called from tud_mount in the main-loop TinyUSB task.
   */
  void Audio_Bridge_Start(void);

  /** @brief Stop I2S DMA and reset stream state. */
  void Audio_Bridge_Stop(void);

  /**
   * @brief Host closed the USB device.
   * @note Silences the buffer and forces the write pointer to re-acquire
   *       its lead on the next stream.
   */
  void Audio_Bridge_StreamStop(void);

  /**
   * @brief Apply master volume to the bound DAC handle.
   * @param vol Attenuation code (0.5 dB steps; see CS4304_SetVolume).
   */
  void Audio_Bridge_SetVolume(uint8_t vol);

  /**
   * @brief Apply mute to the bound DAC handle.
   * @param mute Non-zero to mute.
   */
  void Audio_Bridge_SetMute(uint8_t mute);

  /**
   * @brief No-op. BODY writes are not gated (there is no speaker mute).
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
    AUDIO_CH1_SRC_USB = 0,       /**< Unused in SAMPLE mode (BODY → rings) */
    AUDIO_CH1_SRC_TEST_TONE = 1, /**< CH1 carries SAMPLE note-bank mix */
  } Audio_CH1_Source_t;

  /** Always AUDIO_CH1_SRC_TEST_TONE in SAMPLE mode. */
  Audio_CH1_Source_t Audio_GetCh1Source(void);

  /**
   * @brief I2S1 half-buffer refill hook (no-op: fill runs in the DMA ISR).
   */
  uint32_t Audio_Bridge_UsbDropCount(void);
  void Audio_Bridge_UsbDropCountClear(void);
  uint32_t Audio_Bridge_MaxFill(void);
  uint32_t Audio_Bridge_FillLate(void);

  /**
   * Drive LED_Y low for the complete SPI1 DMA refill and high while idle.
   * This produces a 1 kHz scope signal whose low-duty fraction is refill load.
   */
  void Audio_Bridge_CpuLoadProbeSet(uint8_t enabled);
  uint8_t Audio_Bridge_CpuLoadProbeGet(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_BRIDGE_H__ */
