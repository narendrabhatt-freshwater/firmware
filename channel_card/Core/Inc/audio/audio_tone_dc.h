/**
 ******************************************************************************
 * @file    audio_tone_dc.h
 * @brief   Per-channel sine tone and slewed DC generators (CH1..CH4).
 *
 * CH1 entry exists for the shared table/API but is unused while the note
 * bank owns CH1. CH2–CH4 feed I2S1 right / I2S2 L+R via Audio_ToneDc_NextSample.
 ******************************************************************************
 */

#ifndef __AUDIO_TONE_DC_H__
#define __AUDIO_TONE_DC_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

  typedef enum
  {
    AUDIO_MODE_TONE = 0, /**< Sine wave (default) */
    AUDIO_MODE_DC = 1,   /**< Constant DC level   */
  } Audio_ChannelMode_t;

  /**
   * @brief Set tone frequency for a generator channel (internal; unchecked).
   * @param channel 1..4 (CH1 entry unused while note bank owns CH1).
   * @param freq_hz Frequency in Hz; out-of-range channel is ignored.
   */
  void Audio_SetToneFreq(uint8_t channel, uint32_t freq_hz);

  /**
   * @brief Last tone frequency set for channel.
   * @param channel 1..4.
   * @return Frequency Hz, or 0 if channel invalid.
   */
  uint32_t Audio_GetToneFreq(uint8_t channel);

  /**
   * @brief Select tone vs DC for a generator channel (internal; unchecked).
   * @param channel 1..4.
   * @param mode AUDIO_MODE_TONE or AUDIO_MODE_DC.
   */
  void Audio_SetChannelMode(uint8_t channel, Audio_ChannelMode_t mode);

  /**
   * @brief Current channel mode.
   * @param channel 1..4.
   * @return Mode, or AUDIO_MODE_TONE if channel invalid.
   */
  Audio_ChannelMode_t Audio_GetChannelMode(uint8_t channel);

  /**
   * @brief Set DC level percent for a channel (internal; unchecked).
   * @param channel 1..4.
   * @param percent -100..+100.
   */
  void Audio_SetDCLevel(uint8_t channel, int8_t percent);

  /** @brief Last DC level percent for channel (-100..+100), or 0 if invalid. */
  int8_t Audio_GetDCLevel(uint8_t channel);

  /**
   * @brief Cap |DC| as percent of full scale (internal; unchecked).
   * @param pct_fs Limit 0..100.
   */
  void Audio_SetDCLimit(uint8_t pct_fs);

  /** @brief Current DC |percent| limit. */
  uint8_t Audio_GetDCLimit(void);

  /**
   * @brief Invert DC polarity for a channel (internal; unchecked).
   * @param channel 1..4.
   * @param invert Non-zero to invert.
   */
  void Audio_SetDCInvert(uint8_t channel, uint8_t invert);

  /** @brief Non-zero if DC invert is enabled for channel. */
  uint8_t Audio_GetDCInvert(uint8_t channel);

  /**
   * @brief DC zero calibration offset in percent (internal; unchecked).
   * @param channel 1..4.
   * @param zero_pct Offset percent.
   */
  void Audio_SetDCZero(uint8_t channel, int8_t zero_pct);

  /** @brief Last DC zero offset percent for channel. */
  int8_t Audio_GetDCZero(uint8_t channel);

  /**
   * @brief Fine DC trim in 0.01% units (internal; unchecked).
   * @param channel 1..4.
   * @param trim_x100 Trim ×100.
   */
  void Audio_SetDCTrim(uint8_t channel, int16_t trim_x100);

  /** @brief Last DC trim (×100) for channel. */
  int16_t Audio_GetDCTrim(uint8_t channel);

  /**
   * @brief Next tone or DC sample for one channel (0-based index).
   * @param ch 0..3 (CH1..CH4). CH1 is always USB/note-bank in the bridge;
   *           this is meaningful for CH2..CH4.
   */
  int32_t Audio_ToneDc_NextSample(uint8_t ch);

  /**
   * @brief Zero all tone phases and recompute phase increments from last freqs.
   * @note Called from Audio_StartPlayback() before I2S DMA starts.
   */
  void Audio_ToneDc_ResetPhases(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_TONE_DC_H__ */
