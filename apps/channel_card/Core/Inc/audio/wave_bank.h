/**
 ******************************************************************************
 * @file    wave_bank.h
 * @brief   8-slot one-shot waveform player (int16 in AXI SRAM).
 *
 * Each slot holds up to WAVE_BANK_SAMPLES_MAX int16 LE samples (32 KiB).
 * Playback is one-shot with linear interpolation into the 96 kHz path.
 * Pitch for filter tracking: rate_hz / WAVE_BANK_RATE_PER_HZ (128).
 ******************************************************************************
 */

#ifndef __WAVE_BANK_H__
#define __WAVE_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/** Slots w0..w7 (matches filter console voices 0..7). */
#define WAVE_BANK_SLOTS 8u

/** Bytes per slot / int16 sample count. */
#define WAVE_BANK_BYTES_MAX 32768u
#define WAVE_BANK_SAMPLES_MAX (WAVE_BANK_BYTES_MAX / 2u)

/** Playback rate (samples/s) = pitch_hz * WAVE_BANK_RATE_PER_HZ. */
#define WAVE_BANK_RATE_PER_HZ 128.0

/** Console / API rate bounds (samples per second). */
#define WAVE_BANK_RATE_MIN 1.0
#define WAVE_BANK_RATE_MAX 192000.0

  void WaveBank_Init(void);

  /** Stop all slots and clear playheads (keeps loaded sample data). */
  void WaveBank_StopAll(void);

  /**
   * @brief Replace slot contents with raw int16 LE bytes.
   * @param slot   0..7
   * @param data   Source bytes (may be NULL if nbytes==0)
   * @param nbytes Even, 0..WAVE_BANK_BYTES_MAX
   * @retval 0 success
   * @retval -1 bad slot / odd nbytes / too large / NULL data with nbytes>0
   */
  int WaveBank_Load(uint8_t slot, const uint8_t *data, uint32_t nbytes);

  /** Direct write pointer for streaming upload into an erased/empty slot. */
  int16_t *WaveBank_WritePtr(uint8_t slot);

  /**
   * @brief Commit length after a streaming upload into WritePtr memory.
   * @param nsamp Sample count (0..WAVE_BANK_SAMPLES_MAX)
   */
  int WaveBank_CommitLength(uint8_t slot, uint32_t nsamp);

  uint32_t WaveBank_GetLength(uint8_t slot);
  double WaveBank_GetRate(uint8_t slot);
  uint8_t WaveBank_IsPlaying(uint8_t slot);

  /**
   * @brief Start/restart one-shot at rate samples/s.
   * @retval 0 ok
   * @retval -1 bad slot / empty / rate out of range
   */
  int WaveBank_Trigger(uint8_t slot, double rate_hz);

  /** Hard stop one slot (ignores envelope). */
  void WaveBank_Stop(uint8_t slot);

  /**
   * @brief Note-on from MIDI/nX: rate = freq_hz * 128.
   * @retval 0 ok (same errors as Trigger)
   */
  int WaveBank_NoteOn(uint8_t slot, double freq_hz);

  /** Non-zero if any slot is still advancing its playhead. */
  uint8_t WaveBank_AnyPlaying(void);

  /**
   * Next Q31 sample for slot (advances playhead). Returns 0 if inactive/ended.
   * Hot path — no float except via prior cold-path rate setup.
   */
  int32_t WaveBank_NextSample(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* __WAVE_BANK_H__ */
