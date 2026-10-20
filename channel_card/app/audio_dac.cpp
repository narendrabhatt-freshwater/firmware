/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#include "audio_output.h"
#include "audio.h"
#include <stdint.h>

/* ---- generate control voltage samples ----------------------------------- */

/* CH2..CH4 use indices 1..3; CH1 carries the note-bank mix. */
static int32_t dc_level_tgt[4] = {0, 0, 0, 0}; /* target Q31 sample value */
static int32_t dc_level_now[4] = {0, 0, 0, 0}; /* slewed current value */
/* Limit each sample step to reduce DAC interpolation-filter ringing on
 * the control lines. Traversing -0.5 to +0.5 full scale takes about 85 ms
 * at AUDIO_SAMPLE_RATE_HZ = 48000. */
#define DC_SLEW_STEP (1L << 19)

int32_t render_next_control_voltage_sample(uint8_t ch)
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

static int32_t control_voltage_percent_to_target(uint8_t idx, int8_t percent)
{
    int32_t p = ((dc_invert_mask >> idx) & 1u) ? -percent : percent;
    int32_t z = dc_zero_pct[idx];
    int32_t az = (z < 0) ? -z : z;
    /* effective percent × 100 for precision: z*100 + p*(100-|z|)  (|..| ≤ 10000) */
    int32_t eff_x100 = z * 100 + p * (100 - az) + dc_trim_x100[idx];
    return (int32_t)(((int64_t)0x7FFFFFFF * eff_x100 * dc_fs_limit_pct) / 1000000);
}

/* ---- configure control voltage outputs ---------------------------------- */

void set_control_voltage_target(uint8_t channel, int8_t percent)
{
    if (channel >= 2 && channel <= 4 && percent >= -100 && percent <= 100) {
        uint8_t idx = channel - 1;
        /* The generator slews toward this target by at most DC_SLEW_STEP. */
        dc_level_tgt[idx] = control_voltage_percent_to_target(idx, percent);
    }
}

/* ---- fill control voltage buffers --------------------------------------- */

/**
 * @brief Fill interleaved samples for two control voltage output channels.
 * @param  buf         Pointer to stereo buffer (interleaved L+R, 32-bit)
 * @param  num_frames  Number of stereo frames to generate
 */
void render_control_voltage_channels(int32_t *buf, uint32_t num_frames, uint8_t chL, uint8_t chR)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2] = render_next_control_voltage_sample(chL);
        buf[i * 2 + 1] = render_next_control_voltage_sample(chR);
    }
}

/**
 * @brief  Fill ONE slot (0 = L, 1 = R) of an interleaved stereo buffer with
 *         calibrated DC output, leaving the other slot untouched.
 *         CH2 occupies the right slot; the note-bank mix owns the left slot.
 */
void render_control_voltage_slot(int32_t *buf, uint32_t num_frames, uint8_t ch, uint8_t slot)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2 + slot] = render_next_control_voltage_sample(ch);
    }
}
