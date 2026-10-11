#include "audio.h"
#include "voice.h"
#include "stream.h"
#include <stdint.h>

/* ---- generate control voltage samples ----------------------------------- */

/* CH2..CH4 use indices 1..3; CH1 carries the note-bank mix. */
static int32_t dc_level_tgt[4] = {0, 0, 0, 0}; /* target Q31 sample value */
static int32_t dc_level_now[4] = {0, 0, 0, 0}; /* slewed current value */
/* Limit each sample step to reduce DAC interpolation-filter ringing on
 * the control lines. Traversing -0.5 to +0.5 full scale takes about 85 ms
 * at AUDIO_SAMPLE_RATE_HZ = 48000. */
#define DC_SLEW_STEP (1L << 19)

int32_t Audio_DC_NextSample(uint8_t ch)
{
    int32_t now = dc_level_now[ch];
    int32_t tgt = dc_level_tgt[ch];
    if (now < tgt) {
        now += DC_SLEW_STEP;
        if (now > tgt) {
            now = tgt;
        }
    } else if (now > tgt) {
        now -= DC_SLEW_STEP;
        if (now < tgt) {
            now = tgt;
        }
    }
    dc_level_now[ch] = now;
    return now;
}

/* ---- calibrate control voltage outputs ---------------------------------- */

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

/* ---- convert a dc level to a sample target ------------------------------ */

static int32_t DC_PctToTarget(uint8_t idx, int8_t percent)
{
    int32_t p = ((dc_invert_mask >> idx) & 1u) ? -percent : percent;
    int32_t z = dc_zero_pct[idx];
    int32_t az = (z < 0) ? -z : z;
    /* effective percent × 100 for precision: z*100 + p*(100-|z|)  (|..| ≤ 10000) */
    int32_t eff_x100 = z * 100 + p * (100 - az) + dc_trim_x100[idx];
    return (int32_t)(((int64_t)0x7FFFFFFF * eff_x100 * dc_fs_limit_pct) / 1000000);
}

/* ---- configure control voltage outputs ---------------------------------- */

void Audio_SetDCLevel(uint8_t channel, int8_t percent)
{
    if (channel >= 2 && channel <= 4 && percent >= -100 && percent <= 100) {
        uint8_t idx = channel - 1;
        /* The generator slews toward this target by at most DC_SLEW_STEP. */
        dc_level_tgt[idx] = DC_PctToTarget(idx, percent);
    }
}

/* ---- fill control voltage buffers --------------------------------------- */

/**
 * @brief Fill interleaved samples for two control voltage output channels.
 * @param  buf         Pointer to stereo buffer (interleaved L+R, 32-bit)
 * @param  num_frames  Number of stereo frames to generate
 */
void Audio_FillDC(int32_t *buf, uint32_t num_frames, uint8_t chL, uint8_t chR)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2] = Audio_DC_NextSample(chL);
        buf[i * 2 + 1] = Audio_DC_NextSample(chR);
    }
}

/**
 * @brief  Fill ONE slot (0 = L, 1 = R) of an interleaved stereo buffer with
 *         calibrated DC output, leaving the other slot untouched.
 *         CH2 occupies the right slot; the note-bank mix owns the left slot.
 */
void Audio_FillDCSlot(int32_t *buf, uint32_t num_frames, uint8_t ch, uint8_t slot)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2 + slot] = Audio_DC_NextSample(ch);
    }
}

/* ---- fill the channel audio mix ----------------------------------------- */

/**
 * @brief Fill one slot with the mixed n0–n7 note bank during each DMA refill.
 */
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames, uint8_t slot)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2 + slot] = NoteBank_NextSample();
    }
}

/** CH1 left: always SAMPLE note-bank mix (BODY fills rings, not I2S). */
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames)
{
    Audio_FillCh1NoteBankSlot(buf, num_frames, 0);
}

/* ---- render one audio refill -------------------------------------------- */

volatile uint32_t g_i2s1_fill_late = 0;

void Audio_RenderI2S1(int32_t *buf, uint32_t frames)
{
    NoteBank_VmBoundaryBegin();
    Audio_FillDCSlot(buf, frames, 1, 1);
    Audio_RefillCh1Slot(buf, frames);
    NoteBank_VmBoundaryEnd();
}

/* ---- query audio diagnostics -------------------------------------------- */

uint32_t Audio_Bridge_UsbDropCount(void)
{
    return StreamRing_DropCount();
}

void Audio_Bridge_UsbDropCountClear(void)
{
    StreamRing_StatsClear();
    NoteBank_HoldCountClear();
    g_i2s1_fill_late = 0u;
}

uint32_t Audio_Bridge_MaxFill(void)
{
    return StreamRing_MaxFill();
}

uint32_t Audio_Bridge_FillLate(void)
{
    return g_i2s1_fill_late;
}
