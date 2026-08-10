/**
 ******************************************************************************
 * @file    audio_tone_dc.c
 * @brief   Per-channel sine tone and slewed DC generators (CH1..CH4).
 *
 * Extracted from audio_bridge.c (behavior-preserving). DC slew, zero
 * calibration, and invert mask comments retained.
 ******************************************************************************
 */

#include "audio_tone_dc.h"

#include "audio_rate.h"

#include <stdint.h>

/* Test tone generator — independent frequency per channel (CH1..CH4).
 * All channels share fs = AUDIO_SAMPLE_RATE_HZ (single CS4304 clock domain). */
#define TEST_TONE_SAMPLE_RATE AUDIO_SAMPLE_RATE_HZ
/* CH1 = USB audio (no tone); CH2 = 1 kHz, CH3 = 2 kHz, CH4 = 3 kHz */
static uint32_t test_tone_freq_hz[4] = {1000, 1000, 2000, 3000}; /* CH1..CH4 (CH1 entry unused) */
static uint32_t test_tone_phase[4] = {0, 0, 0, 0};
/* Phase increment per sample (fixed-point 32-bit, full scale = 2^32),
 * precomputed per channel so ISRs never do 64-bit divides. */
#define TONE_PHASE_INC(f) ((uint32_t)(((uint64_t)(f) << 32) / TEST_TONE_SAMPLE_RATE))
static uint32_t test_tone_inc[4] = {0, 0, 0, 0};

/* --- Per-channel output mode: TONE (sine) or DC (constant level) ---
 * CH1 is always USB audio; modes only apply to CH2..CH4 generation. */
static Audio_ChannelMode_t channel_mode[4] = {AUDIO_MODE_TONE, AUDIO_MODE_TONE,
                                              AUDIO_MODE_TONE, AUDIO_MODE_TONE};
static int8_t dc_level_pct[4] = {0, 0, 0, 0};  /* -100..+100 percent (signed!) */
static int32_t dc_level_tgt[4] = {0, 0, 0, 0}; /* target Q31 sample value */
static int32_t dc_level_now[4] = {0, 0, 0, 0}; /* slewed current value */
/* Slew rate: full range (±0.5 FS) in ~21 ms at 96 kHz. An instant step
 * rings the DAC's interpolation filter → spikes on the control line. */
#define DC_SLEW_STEP (1L << 19)

/* Simple sine lookup table (256 entries, Q31 format) */
#define SINE_TABLE_SIZE 256
static const int32_t sine_table[SINE_TABLE_SIZE] = {
    0x00000000,
    0x0323ECBE,
    0x0647D97C,
    0x096A9049,
    0x0C8BD35E,
    0x0FAB272B,
    0x12C8106F,
    0x15E21445,
    0x18F8B83C,
    0x1C0B826A,
    0x1F19F97B,
    0x2223A4C5,
    0x25280C5E,
    0x2826B928,
    0x2B1F34EB,
    0x2E110A62,
    0x30FBC54D,
    0x33DEF287,
    0x36BA2014,
    0x398CDD32,
    0x3C56BA70,
    0x3F1749B8,
    0x41CE1E65,
    0x447ACD50,
    0x471CECE7,
    0x49B41533,
    0x4C3FDFF4,
    0x4EBFE8A5,
    0x5133CC94,
    0x539B2AF0,
    0x55F5A4D2,
    0x5842DD54,
    0x5A82799A,
    0x5CB420E0,
    0x5ED77C8A,
    0x60EC3830,
    0x62F201AC,
    0x64E88926,
    0x66CF8120,
    0x68A69E81,
    0x6A6D98A4,
    0x6C242960,
    0x6DCA0D14,
    0x6F5F02B2,
    0x70E2CBC6,
    0x72552C85,
    0x73B5EBD1,
    0x7504D345,
    0x7641AF3D,
    0x776C4EDB,
    0x78848414,
    0x798A23B1,
    0x7A7D055B,
    0x7B5D039E,
    0x7C29FBEE,
    0x7CE3CEB2,
    0x7D8A5F40,
    0x7E1D93EA,
    0x7E9D55FC,
    0x7F0991C4,
    0x7F62368F,
    0x7FA736B4,
    0x7FD8878E,
    0x7FF62182,
    0x7FFFFFFF,
    0x7FF62182,
    0x7FD8878E,
    0x7FA736B4,
    0x7F62368F,
    0x7F0991C4,
    0x7E9D55FC,
    0x7E1D93EA,
    0x7D8A5F40,
    0x7CE3CEB2,
    0x7C29FBEE,
    0x7B5D039E,
    0x7A7D055B,
    0x798A23B1,
    0x78848414,
    0x776C4EDB,
    0x7641AF3D,
    0x7504D345,
    0x73B5EBD1,
    0x72552C85,
    0x70E2CBC6,
    0x6F5F02B2,
    0x6DCA0D14,
    0x6C242960,
    0x6A6D98A4,
    0x68A69E81,
    0x66CF8120,
    0x64E88926,
    0x62F201AC,
    0x60EC3830,
    0x5ED77C8A,
    0x5CB420E0,
    0x5A82799A,
    0x5842DD54,
    0x55F5A4D2,
    0x539B2AF0,
    0x5133CC94,
    0x4EBFE8A5,
    0x4C3FDFF4,
    0x49B41533,
    0x471CECE7,
    0x447ACD50,
    0x41CE1E65,
    0x3F1749B8,
    0x3C56BA70,
    0x398CDD32,
    0x36BA2014,
    0x33DEF287,
    0x30FBC54D,
    0x2E110A62,
    0x2B1F34EB,
    0x2826B928,
    0x25280C5E,
    0x2223A4C5,
    0x1F19F97B,
    0x1C0B826A,
    0x18F8B83C,
    0x15E21445,
    0x12C8106F,
    0x0FAB272B,
    0x0C8BD35E,
    0x096A9049,
    0x0647D97C,
    0x0323ECBE,
    0x00000000,
    0xFCDC1342,
    0xF9B82684,
    0xF6956FB7,
    0xF3742CA2,
    0xF054D8D5,
    0xED37EF91,
    0xEA1DEBBB,
    0xE70747C4,
    0xE3F47D96,
    0xE0E60685,
    0xDDDC5B3B,
    0xDAD7F3A2,
    0xD7D946D8,
    0xD4E0CB15,
    0xD1EEF59E,
    0xCF043AB3,
    0xCC210D79,
    0xC945DFEC,
    0xC67322CE,
    0xC3A94590,
    0xC0E8B648,
    0xBE31E19B,
    0xBB8532B0,
    0xB8E31319,
    0xB64BEACD,
    0xB3C0200C,
    0xB140175B,
    0xAECC336C,
    0xAC64D510,
    0xAA0A5B2E,
    0xA7BD22AC,
    0xA57D8666,
    0xA34BDF20,
    0xA1288376,
    0x9F13C7D0,
    0x9D0DFE54,
    0x9B1776DA,
    0x99307EE0,
    0x9759617F,
    0x9592675C,
    0x93DBD6A0,
    0x9235F2EC,
    0x90A0FD4E,
    0x8F1D343A,
    0x8DAAD37B,
    0x8C4A142F,
    0x8AFB2CBB,
    0x89BE50C3,
    0x8893B125,
    0x877B7BEC,
    0x8675DC4F,
    0x8782FAA5,
    0x84A2FC62,
    0x83D60412,
    0x831C314E,
    0x8275A0C0,
    0x81E26C16,
    0x8162AA04,
    0x80F66E3C,
    0x809DC971,
    0x8058C94C,
    0x80277872,
    0x8009DE7E,
    0x80000001,
    0x8009DE7E,
    0x80277872,
    0x8058C94C,
    0x809DC971,
    0x80F66E3C,
    0x8162AA04,
    0x81E26C16,
    0x8275A0C0,
    0x831C314E,
    0x83D60412,
    0x84A2FC62,
    0x8782FAA5,
    0x8675DC4F,
    0x877B7BEC,
    0x8893B125,
    0x89BE50C3,
    0x8AFB2CBB,
    0x8C4A142F,
    0x8DAAD37B,
    0x8F1D343A,
    0x90A0FD4E,
    0x9235F2EC,
    0x93DBD6A0,
    0x9592675C,
    0x9759617F,
    0x99307EE0,
    0x9B1776DA,
    0x9D0DFE54,
    0x9F13C7D0,
    0xA1288376,
    0xA34BDF20,
    0xA57D8666,
    0xA7BD22AC,
    0xAA0A5B2E,
    0xAC64D510,
    0xAECC336C,
    0xB140175B,
    0xB3C0200C,
    0xB64BEACD,
    0xB8E31319,
    0xBB8532B0,
    0xBE31E19B,
    0xC0E8B648,
    0xC3A94590,
    0xC67322CE,
    0xC945DFEC,
    0xCC210D79,
    0xCF043AB3,
    0xD1EEF59E,
    0xD4E0CB15,
    0xD7D946D8,
    0xDAD7F3A2,
    0xDDDC5B3B,
    0xE0E60685,
    0xE3F47D96,
    0xE70747C4,
    0xEA1DEBBB,
    0xED37EF91,
    0xF054D8D5,
    0xF3742CA2,
    0xF6956FB7,
    0xF9B82684,
    0xFCDC1342,
};

/** Next tone sample for one channel, with LINEAR INTERPOLATION between
 * sine-table entries. Plain 8-bit table lookup quantizes the phase to
 * 1/256 cycle, making zero-crossings jitter by up to ~4 µs at 1 kHz
 * ("shaky" waveform on the scope). Interpolation removes it. -6 dBFS.
 * Cost: ~10 cycles/sample — negligible at 550 MHz. */
static int32_t Tone_NextSineSample(uint8_t ch)
{
  uint32_t ph = test_tone_phase[ch];
  uint8_t idx = (uint8_t)(ph >> 24);
  int32_t s0 = sine_table[idx];
  int32_t s1 = sine_table[(uint8_t)(idx + 1u)];
  uint32_t frac = (ph >> 8) & 0xFFFFu; /* 16-bit fraction */
  int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) >> 16);

  test_tone_phase[ch] = ph + test_tone_inc[ch];
  return s >> 1; /* -6 dB headroom */
}

/** Next sample for one channel — returns sine tone or DC level depending
 * on the channel mode. CH1 (index 0) is always USB audio so this is
 * only meaningful for CH2..CH4 (indices 1..3). */
int32_t Audio_ToneDc_NextSample(uint8_t ch)
{
  if (channel_mode[ch] == AUDIO_MODE_DC)
  {
    /* Slew-limited approach to the target: no steps, no filter ringing */
    int32_t now = dc_level_now[ch];
    int32_t tgt = dc_level_tgt[ch];
    if (now < tgt)
    {
      now += DC_SLEW_STEP;
      if (now > tgt)
      {
        now = tgt;
      }
    }
    else if (now > tgt)
    {
      now -= DC_SLEW_STEP;
      if (now < tgt)
      {
        now = tgt;
      }
    }
    dc_level_now[ch] = now;
    return now;
  }
  return Tone_NextSineSample(ch);
}

void Audio_ToneDc_ResetPhases(void)
{
  test_tone_phase[0] = test_tone_phase[1] = 0;
  test_tone_phase[2] = test_tone_phase[3] = 0;
  for (uint8_t i = 0; i < 4; i++)
  {
    test_tone_inc[i] = TONE_PHASE_INC(test_tone_freq_hz[i]);
  }
}

void Audio_SetToneFreq(uint8_t channel, uint32_t freq_hz)
{
  if (channel >= 1 && channel <= 4 && freq_hz > 0 && freq_hz < 20000)
  {
    test_tone_freq_hz[channel - 1] = freq_hz;
    test_tone_inc[channel - 1] = TONE_PHASE_INC(freq_hz);
  }
}

uint32_t Audio_GetToneFreq(uint8_t channel)
{
  return (channel >= 1 && channel <= 4) ? test_tone_freq_hz[channel - 1] : 0;
}

void Audio_SetChannelMode(uint8_t channel, Audio_ChannelMode_t mode)
{
  if (channel >= 2 && channel <= 4)
  {
    channel_mode[channel - 1] = mode;
  }
}

Audio_ChannelMode_t Audio_GetChannelMode(uint8_t channel)
{
  return (channel >= 1 && channel <= 4) ? channel_mode[channel - 1] : AUDIO_MODE_TONE;
}

/* Symmetric DC range cap, in % of DAC full scale. The analog chain rides on
 * VMID, so headroom is asymmetric — the smaller (negative-going) side sets
 * the usable range and BOTH polarities are capped to it, keeping the output
 * symmetric about the VMID center. Positive headroom above the cap is
 * intentionally unused. */
static uint8_t dc_fs_limit_pct = 50;

/* Per-channel sign inversion (bit = channel index). The analog conditioning
 * stages are inverting, so this makes a positive requested level produce a
 * POSITIVE voltage at the final control point regardless of chain sign. */
static uint8_t dc_invert_mask = 0;

/* Per-channel calibrated zero: the dc-code (in % units) at which the ANALOG
 * output crosses 0 V (the VMID offset seen through the conditioning stage).
 * With zero Z set, requested percent p maps to:  eff = Z + p*(100-|Z|)/100
 *   p = 0    -> true 0 V at the output
 *   p = +100 -> full reach on the wide side
 *   p = -100 -> the voltage MIRROR of +100 around the new zero
 * so the control is symmetric in physical volts about 0 V. */
/* Board calibration (measured 2026-07-13): output crosses 0 V at these
 * codes — CH2: +32, CH3: +22, CH4: +32. Re-measure if VMID or the
 * conditioning stages change. */
static int8_t dc_zero_pct[4] = {0, 32, 22, 32};

/* Fine zero trim in 0.01% units (±9.99%), added on top of dc_zero_pct.
 * Replaces an analog trimmer: with the 50% range cap one step ≈ 0.13 mV at
 * the output — nulls diff-amp resistor-tolerance residuals digitally. */
static int16_t dc_trim_x100[4] = {0, 0, 0, 0};

static int32_t DC_PctToTarget(uint8_t idx, int8_t percent)
{
  int32_t p = ((dc_invert_mask >> idx) & 1u) ? -percent : percent;
  int32_t z = dc_zero_pct[idx];
  int32_t az = (z < 0) ? -z : z;
  /* effective percent × 100 for precision: z*100 + p*(100-|z|)  (|..| ≤ 10000) */
  int32_t eff_x100 = z * 100 + p * (100 - az) + dc_trim_x100[idx];
  return (int32_t)(((int64_t)0x7FFFFFFF * eff_x100 * dc_fs_limit_pct) / 1000000);
}

void Audio_SetDCLevel(uint8_t channel, int8_t percent)
{
  if (channel >= 2 && channel <= 4 && percent >= -100 && percent <= 100)
  {
    uint8_t idx = channel - 1;
    dc_level_pct[idx] = percent;
    /* The generator slews toward this target (DC_SLEW_STEP) — no spikes. */
    dc_level_tgt[idx] = DC_PctToTarget(idx, percent);
    channel_mode[idx] = AUDIO_MODE_DC; /* auto-switch to DC mode */
  }
}

void Audio_SetDCLimit(uint8_t pct_fs)
{
  if (pct_fs >= 5 && pct_fs <= 100)
  {
    dc_fs_limit_pct = pct_fs;
    /* Re-scale all active DC channels to the new cap (slewed, no spikes) */
    for (uint8_t i = 1; i < 4; i++)
    {
      if (channel_mode[i] == AUDIO_MODE_DC)
      {
        dc_level_tgt[i] = DC_PctToTarget(i, dc_level_pct[i]);
      }
    }
  }
}

uint8_t Audio_GetDCLimit(void)
{
  return dc_fs_limit_pct;
}

void Audio_SetDCInvert(uint8_t channel, uint8_t invert)
{
  if (channel >= 2 && channel <= 4)
  {
    uint8_t idx = channel - 1;
    if (invert)
    {
      dc_invert_mask |= (uint8_t)(1u << idx);
    }
    else
    {
      dc_invert_mask &= (uint8_t)~(1u << idx);
    }
    if (channel_mode[idx] == AUDIO_MODE_DC)
    {
      dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]); /* re-apply */
    }
  }
}

uint8_t Audio_GetDCInvert(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? ((dc_invert_mask >> (channel - 1)) & 1u) : 0;
}

void Audio_SetDCZero(uint8_t channel, int8_t zero_pct)
{
  if (channel >= 2 && channel <= 4 && zero_pct >= -99 && zero_pct <= 99)
  {
    uint8_t idx = channel - 1;
    dc_zero_pct[idx] = zero_pct;
    /* Calibrating the zero implies DC output: switch mode and apply
     * immediately so the effect is always visible (like Audio_SetDCLevel). */
    channel_mode[idx] = AUDIO_MODE_DC;
    dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]);
  }
}

int8_t Audio_GetDCZero(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_zero_pct[channel - 1] : 0;
}

void Audio_SetDCTrim(uint8_t channel, int16_t trim_x100)
{
  if (channel >= 2 && channel <= 4 && trim_x100 >= -999 && trim_x100 <= 999)
  {
    uint8_t idx = channel - 1;
    dc_trim_x100[idx] = trim_x100;
    channel_mode[idx] = AUDIO_MODE_DC; /* trimming implies DC output */
    dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]);
  }
}

int16_t Audio_GetDCTrim(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_trim_x100[channel - 1] : 0;
}

int8_t Audio_GetDCLevel(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_level_pct[channel - 1] : 0;
}
