/**
 ******************************************************************************
 * @file    note_bank.h
 * @brief   16-voice (N0–NF) additive sine bank for Channel Card CH1.
 *
 * Each voice is an independent fixed-point phase-accumulator oscillator.
 * Hot-path samples are summed and saturated into one Q31 sample for the
 * CH1 (I2S1 left) slot. Cold-path frequency changes use double only to
 * compute the phase increment — never in DMA/ISR code.
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

    /**
     * Set one note's frequency in Hz (fractional OK, e.g. 440.5).
     * note: 0..15 (N0..NF). Out-of-range note is ignored.
     * freq_hz <= 0 turns that note off (inc = 0); does not affect other notes.
     * Caller should bounds-check audible range (e.g. 20..19999.9) before calling.
     */
    void NoteBank_SetFreq(uint8_t note, double freq_hz);

    /** Last frequency set for `note`, in Hz. 0 if off or note invalid. */
    double NoteBank_GetFreq(uint8_t note);

    /** Non-zero if any note has a non-zero phase increment (CH1 should play bank). */
    uint8_t NoteBank_AnyActive(void);

    /**
     * Next mixed Q31 sample: sum active voices, saturate to int32_t.
     * Hot path — integer only. Safe to call from DMA callbacks.
     */
    int32_t NoteBank_NextSample(void);

#ifdef __cplusplus
}
#endif

#endif /* __NOTE_BANK_H__ */
