/**
 ******************************************************************************
 * @file    note_bank.h
 * @brief   16-voice (N0–NF) additive oscillator bank for Channel Card CH1.
 *
 * Each voice is an independent fixed-point phase-accumulator oscillator with
 * its own amplitude scale (0.0..1.0), optional multi-segment envelope
 * (note_envelope.h), and optional 4-pole Butterworth LPF (note_filter.h).
 * All voices share one global shape (sine / pulse / triangle). Hot-path
 * samples are amplitude-scaled, filtered, summed, and saturated into one
 * Q31 sample for the CH1 (I2S1 left) slot.
 * Cold-path frequency/scale/shape use double only to compute phase inc /
 * Q15 amp / duty thresholds — never in DMA/ISR code.
 ******************************************************************************
 */

#ifndef __NOTE_BANK_H__
#define __NOTE_BANK_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/** Number of simultaneous notes: N0..NF (hex slots 0..15). */
#define NOTE_BANK_VOICES 16u

  /** Global oscillator shape for all N0–NF voices. */
  typedef enum
  {
    NOTE_SHAPE_SINE = 0,
    NOTE_SHAPE_PULSE = 1,
    NOTE_SHAPE_TRI = 2
  } NoteBank_Shape_t;

  /**
   * @brief Establish known-off state for all voices (sine shape, zero freq).
   * @note Call once at bring-up after NoteFilter_InitAll / NoteEnv_Init.
   */
  void NoteBank_Init(void);

  /**
   * @brief Set one note's frequency (Hz) and amplitude scale (0.0..1.0).
   * @param note    Voice 0..15 (N0..NF). Out-of-range is ignored.
   * @param freq_hz Frequency Hz; <= 0 turns the note off (release if env active).
   * @param scale   Amplitude 0.0..1.0 (clamped). Ignored when off.
   * @note Unchecked internal API for Hz range — console must reject OOR first.
   *       scale clamp is intentional for this cold-path setter.
   */
  void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale);

  /** Last frequency set for `note`, in Hz. 0 if off or note invalid. */
  double NoteBank_GetFreq(uint8_t note);

  /** Last amplitude scale for `note` (0.0..1.0). 0 if note invalid. */
  double NoteBank_GetScale(uint8_t note);

  /**
   * Select global shape for all voices.
   * Sine ignores `param`. Pulse/tri require param in [0.1, 0.9] (duty /
   * triangle asymmetry). Returns 0 on success, -1 on bad shape/param.
   * Does not touch frequency, scale, gain, or filter cutoff.
   */
  int NoteBank_SetShape(NoteBank_Shape_t shape, double param);

  /** Current global shape (default sine). */
  NoteBank_Shape_t NoteBank_GetShape(void);

  /** Last pulse/tri param (0.1..0.9). Meaningful when those shapes are active. */
  double NoteBank_GetShapeParam(void);

  /** Non-zero if any note has a non-zero phase increment (CH1 should play bank). */
  uint8_t NoteBank_AnyActive(void);

  /**
   * Next mixed Q31 sample: sum amplitude-scaled active voices, saturate.
   * Hot path — integer osc + float LPF boundary. Safe from DMA callbacks.
   */
  int32_t NoteBank_NextSample(void);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_BANK_H__ */
