/**
 ******************************************************************************
 * @file    note_bank.h
 * @brief   16-voice (N0–NF) additive sine bank for Channel Card CH1.
 *
 * Each voice is an independent fixed-point phase-accumulator sine with its
 * own amplitude scale (0.0..1.0) and optional 4-pole Butterworth LPF
 * (see note_filter.h). Hot-path samples are amplitude-scaled, filtered,
 * summed, and saturated into one Q31 sample for the CH1 (I2S1 left) slot.
 * Cold-path frequency/scale use double only to compute phase inc / Q15 amp —
 * never in DMA/ISR code.
 ******************************************************************************
 */

#ifndef __NOTE_BANK_H__
#define __NOTE_BANK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Number of simultaneous notes: N0..NF (hex slots 0..15). */
#define NOTE_BANK_VOICES 16u

/**
 * Set one note's frequency (Hz) and amplitude scale (0.0..1.0).
 * note: 0..15 (N0..NF). Out-of-range note is ignored.
 * freq_hz <= 0 turns that note off (inc = 0); scale is ignored when off.
 * scale is clamped to 0.0..1.0; 1.0 = full table amplitude.
 * Caller should bounds-check audible Hz (e.g. 20..19999.9) before calling.
 */
void NoteBank_SetFreq(uint8_t note, double freq_hz, double scale);

/** Last frequency set for `note`, in Hz. 0 if off or note invalid. */
double NoteBank_GetFreq(uint8_t note);

/** Last amplitude scale for `note` (0.0..1.0). 0 if note invalid. */
double NoteBank_GetScale(uint8_t note);

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
