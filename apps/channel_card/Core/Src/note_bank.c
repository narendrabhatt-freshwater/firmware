/**
 ******************************************************************************
 * @file    note_bank.c
 * @brief   16-voice additive DDS bank for Channel Card CH1 (N0–NF).
 ******************************************************************************
 */

#include "note_bank.h"

#include <stdint.h>

/* Must match I2S / CS4304 sample rate (see audio-dsp-conventions.mdc). */
#define NOTE_BANK_SAMPLE_RATE 96000u

/*
 * Dedicated 128-entry Q31 sine table for the note bank (same table formerly
 * used by the monophonic CH1 test tone). Independent of the 256-entry table
 * used by CH2–4 in audio_bridge.c.
 *
 * Table size sets interpolation error only; the 32-bit phase accumulator
 * gives ~0.0000223 Hz resolution at 96 kHz regardless.
 */
#define NOTE_SINE_TABLE_SIZE 128u
static const int32_t note_sine_table[NOTE_SINE_TABLE_SIZE] = {
    0x00000000,
    0x0647D97C,
    0x0C8BD35E,
    0x12C8106E,
    0x18F8B83C,
    0x1F19F97B,
    0x25280C5D,
    0x2B1F34EB,
    0x30FBC54D,
    0x36BA2013,
    0x3C56BA70,
    0x41CE1E64,
    0x471CECE6,
    0x4C3FDFF3,
    0x5133CC94,
    0x55F5A4D2,
    0x5A827999,
    0x5ED77C89,
    0x62F201AC,
    0x66CF811F,
    0x6A6D98A3,
    0x6DCA0D14,
    0x70E2CBC5,
    0x73B5EBD0,
    0x7641AF3C,
    0x78848413,
    0x7A7D055A,
    0x7C29FBED,
    0x7D8A5F3F,
    0x7E9D55FB,
    0x7F62368E,
    0x7FD8878D,
    0x7FFFFFFF,
    0x7FD8878D,
    0x7F62368E,
    0x7E9D55FB,
    0x7D8A5F3F,
    0x7C29FBED,
    0x7A7D055A,
    0x78848413,
    0x7641AF3C,
    0x73B5EBD0,
    0x70E2CBC5,
    0x6DCA0D14,
    0x6A6D98A3,
    0x66CF811F,
    0x62F201AC,
    0x5ED77C89,
    0x5A827999,
    0x55F5A4D2,
    0x5133CC94,
    0x4C3FDFF3,
    0x471CECE6,
    0x41CE1E64,
    0x3C56BA70,
    0x36BA2013,
    0x30FBC54D,
    0x2B1F34EB,
    0x25280C5D,
    0x1F19F97B,
    0x18F8B83C,
    0x12C8106E,
    0x0C8BD35E,
    0x0647D97C,
    0x00000000,
    0xF9B82684,
    0xF3742CA2,
    0xED37EF92,
    0xE70747C4,
    0xE0E60685,
    0xDAD7F3A3,
    0xD4E0CB15,
    0xCF043AB3,
    0xC945DFED,
    0xC3A94590,
    0xBE31E19C,
    0xB8E3131A,
    0xB3C0200D,
    0xAECC336C,
    0xAA0A5B2E,
    0xA57D8667,
    0xA1288377,
    0x9D0DFE54,
    0x99307EE1,
    0x9592675D,
    0x9235F2EC,
    0x8F1D343B,
    0x8C4A1430,
    0x89BE50C4,
    0x877B7BED,
    0x8582FAA6,
    0x83D60413,
    0x8275A0C1,
    0x8162AA05,
    0x809DC972,
    0x80277873,
    0x80000001,
    0x80277873,
    0x809DC972,
    0x8162AA05,
    0x8275A0C1,
    0x83D60413,
    0x8582FAA6,
    0x877B7BED,
    0x89BE50C4,
    0x8C4A1430,
    0x8F1D343B,
    0x9235F2EC,
    0x9592675D,
    0x99307EE1,
    0x9D0DFE54,
    0xA1288377,
    0xA57D8667,
    0xAA0A5B2E,
    0xAECC336C,
    0xB3C0200D,
    0xB8E3131A,
    0xBE31E19C,
    0xC3A94590,
    0xC945DFED,
    0xCF043AB3,
    0xD4E0CB15,
    0xDAD7F3A3,
    0xE0E60685,
    0xE70747C4,
    0xED37EF92,
    0xF3742CA2,
    0xF9B82684,
};

/* Cold-path Hz (for GetFreq); hot path uses phase/inc only. */
static double note_freq_hz[NOTE_BANK_VOICES];
static uint32_t note_phase[NOTE_BANK_VOICES];
static uint32_t note_inc[NOTE_BANK_VOICES];

/** Cold path only — convert Hz to 32-bit phase increment at NOTE_BANK_SAMPLE_RATE. */
static uint32_t NoteBank_PhaseIncFromHz(double freq_hz)
{
  double inc = (freq_hz * 4294967296.0) / (double)NOTE_BANK_SAMPLE_RATE;
  return (uint32_t)(inc + 0.5);
}

/**
 * One voice: 128-entry table, linear interp, -6 dBFS (matches former CH1 tone).
 * Pure integer; ~10 cycles — fine inside the DMA half refill.
 */
static inline int32_t NoteBank_VoiceSample(uint8_t note)
{
  uint32_t ph = note_phase[note];
  uint8_t idx = (uint8_t)(ph >> 25); /* 7-bit index, 0..127 */
  int32_t s0 = note_sine_table[idx];
  int32_t s1 =
      note_sine_table[(uint8_t)((idx + 1u) & (NOTE_SINE_TABLE_SIZE - 1u))];
  uint32_t frac = (ph >> 9) & 0xFFFFu;
  int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) >> 16);

  note_phase[note] = ph + note_inc[note];
  return s >> 1;
}

/** Saturate a 64-bit mix sum into Q31. */
static inline int32_t NoteBank_Saturate(int64_t sum)
{
  if (sum > (int64_t)0x7FFFFFFF)
  {
    return (int32_t)0x7FFFFFFF;
  }
  if (sum < (int64_t)(int32_t)0x80000000)
  {
    return (int32_t)0x80000000;
  }
  return (int32_t)sum;
}

void NoteBank_SetFreq(uint8_t note, double freq_hz)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return;
  }

  if (freq_hz <= 0.0)
  {
    note_freq_hz[note] = 0.0;
    note_inc[note] = 0;
    return;
  }

  note_freq_hz[note] = freq_hz;
  note_inc[note] = NoteBank_PhaseIncFromHz(freq_hz);
}

double NoteBank_GetFreq(uint8_t note)
{
  if (note >= NOTE_BANK_VOICES)
  {
    return 0.0;
  }
  return note_freq_hz[note];
}

uint8_t NoteBank_AnyActive(void)
{
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    if (note_inc[i] != 0u)
    {
      return 1u;
    }
  }
  return 0u;
}

int32_t NoteBank_NextSample(void)
{
  int64_t sum = 0;
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    if (note_inc[i] != 0u)
    {
      sum += (int64_t)NoteBank_VoiceSample(i);
    }
  }
  return NoteBank_Saturate(sum);
}
