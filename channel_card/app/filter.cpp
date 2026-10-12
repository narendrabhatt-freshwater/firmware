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

#include "filter.h"
#include "audio.h"
#include <math.h>
#include <stdint.h>

/* ---- design and process four-pole filters ------------------------------- */

/* 4-pole DF4 Butterworth kernel, based on the reference four_pole_filter. */

/* ---- convert cutoff frequency to angular frequency ---------------------- */

double cutoff_to_omega(double cutoff_hz, double sample_rate)
{
    const double pi = 3.14159265358979323846;
    double nyquist;

    if (cutoff_hz < 0.0) {
        cutoff_hz = 0.0;
    }
    nyquist = 0.5 * sample_rate;
    if (cutoff_hz > 0.999 * nyquist) {
        cutoff_hz = 0.999 * nyquist;
    }
    return 2.0 * pi * cutoff_hz / sample_rate;
}

/* ---- reset filter history ----------------------------------------------- */

void reset_four_pole_filter(ButterFourPole_t *f)
{
    if (f == 0) {
        return;
    }
    f->d[0] = 0.0;
    f->d[1] = 0.0;
    f->d[2] = 0.0;
    f->d[3] = 0.0;
}

/* ---- calculate low-pass coefficients ------------------------------------ */

void init_four_pole_low_pass_filter(ButterFourPole_t *f, double omega, double g)
{
    double k;
    double p;
    double q;
    double a;
    double a0;
    double a1;
    double a2;
    double a3;
    double a4;

    if (f == 0) {
        return;
    }

    k = (4.0 * g - 3.0) / (g + 1.0);
    p = 1.0 - 0.25 * k;
    p *= p;

    a = 1.0 / (tan(0.5 * omega) * (1.0 + p));
    p = 1.0 + a;
    q = 1.0 - a;

    a0 = 1.0 / (k + p * p * p * p);
    a1 = 4.0 * (k + p * p * p * q);
    a2 = 6.0 * (k + p * p * q * q);
    a3 = 4.0 * (k + p * q * q * q);
    a4 = (k + q * q * q * q);
    p = a0 * (k + 1.0);

    f->coef[0] = p;
    f->coef[1] = 4.0 * p;
    f->coef[2] = 6.0 * p;
    f->coef[3] = 4.0 * p;
    f->coef[4] = p;
    f->coef[5] = -a1 * a0;
    f->coef[6] = -a2 * a0;
    f->coef[7] = -a3 * a0;
    f->coef[8] = -a4 * a0;

    reset_four_pole_filter(f);
}

/* ---- calculate high-pass coefficients ----------------------------------- */

void init_four_pole_high_pass_filter(ButterFourPole_t *f, double omega, double g)
{
    double k;
    double p;
    double q;
    double a;
    double a0;
    double a1;
    double a2;
    double a3;
    double a4;

    if (f == 0) {
        return;
    }

    k = (4.0 * g - 3.0) / (g + 1.0);
    p = 1.0 - 0.25 * k;
    p *= p;

    a = tan(0.5 * omega) / (1.0 + p);
    p = a + 1.0;
    q = a - 1.0;

    a0 = 1.0 / (p * p * p * p + k);
    a1 = 4.0 * (p * p * p * q - k);
    a2 = 6.0 * (p * p * q * q + k);
    a3 = 4.0 * (p * q * q * q - k);
    a4 = (q * q * q * q + k);
    p = a0 * (k + 1.0);

    f->coef[0] = p;
    f->coef[1] = -4.0 * p;
    f->coef[2] = 6.0 * p;
    f->coef[3] = -4.0 * p;
    f->coef[4] = p;
    f->coef[5] = -a1 * a0;
    f->coef[6] = -a2 * a0;
    f->coef[7] = -a3 * a0;
    f->coef[8] = -a4 * a0;

    reset_four_pole_filter(f);
}

/* ---- filter one sample -------------------------------------------------- */

double filter_four_pole_sample(ButterFourPole_t *f, double in)
{
    double out;

    if (f == 0) {
        return in;
    }

    out = f->coef[0] * in + f->d[0];
    f->d[0] = f->coef[1] * in + f->coef[5] * out + f->d[1];
    f->d[1] = f->coef[2] * in + f->coef[6] * out + f->d[2];
    f->d[2] = f->coef[3] * in + f->coef[7] * out + f->d[3];
    f->d[3] = f->coef[4] * in + f->coef[8] * out;
    return out;
}

/* ---- filter individual voices ------------------------------------------- */

/* Per-voice 4-pole Butterworth LPF wrapper for the note bank.
 * Owns voice base/effective cutoff, pitch-track k, bypass/q tables and Q31
 * edges. The DF4 kernel above uses floating point in the sample path.
 * Console q maps to DF4 g (1.0 ≈
 * Butterworth); higher → more peak near fc.
 * Key follow (CMI-style): fc = fbase * (f_note/C4)^k = fbase * 2^(k n/12).
 * f0 programs fbase at C4; fk sets k; OnNoteFreq retunes on note change.
 * HP/BP deferred at the NoteFilter API. */

/* Must match I2S, CS4304, and the note-bank sample rate in audio.h. */
#define NOTE_FILTER_SAMPLE_RATE ((double)AUDIO_SAMPLE_RATE_HZ)

/** Openest designed corner without entering bypass (== MAX). */
#define NOTE_FILTER_DESIGN_MAX_HZ (NOTE_FILTER_CUTOFF_MAX_HZ - 1.0)

static double s_base_hz[NOTE_FILTER_VOICES];
static double s_eff_hz[NOTE_FILTER_VOICES];
static double s_q[NOTE_FILTER_VOICES];
static double s_k[NOTE_FILTER_VOICES];
static double s_last_note_hz[NOTE_FILTER_VOICES];
static uint8_t s_bypass[NOTE_FILTER_VOICES];
static ButterFourPole_t s_filt[NOTE_FILTER_VOICES];

/* ---- convert and constrain filter values -------------------------------- */

static int32_t filter_sample_to_q31(double x)
{
    if (x >= 1.0) {
        return (int32_t)0x7FFFFFFF;
    }
    if (x <= -1.0) {
        return (int32_t)0x80000001;
    }
    return (int32_t)(x * 2147483647.0);
}

static double clamp_filter_design_frequency_hz(double fc)
{
    if (fc < NOTE_FILTER_CUTOFF_MIN_HZ) {
        return NOTE_FILTER_CUTOFF_MIN_HZ;
    }
    if (fc > NOTE_FILTER_DESIGN_MAX_HZ) {
        return NOTE_FILTER_DESIGN_MAX_HZ;
    }
    return fc;
}

/**
 * Effective fc from base + pitch-track.
 * k==0 or unknown note → base. Track never sets the bypass flag.
 */
static double calculate_voice_filter_cutoff_hz(uint8_t voice)
{
    double base;
    double k;
    double note;
    double ratio;
    double fc;

    base = s_base_hz[voice];
    k = s_k[voice];
    note = s_last_note_hz[voice];

    if (k <= 0.0 || !(note > 0.0)) {
        return clamp_filter_design_frequency_hz(base);
    }

    ratio = note / NOTE_FILTER_F_REF_HZ;
    if (!(ratio > 0.0)) {
        return clamp_filter_design_frequency_hz(base);
    }

    fc = base * pow(ratio, k);
    return clamp_filter_design_frequency_hz(fc);
}

/* ---- update filter coefficients ----------------------------------------- */

/** Redesign LPF from effective cutoff + q. Caller: voice valid, not bypass. */
static void calculate_voice_filter_coefficients(uint8_t voice)
{
    double omega;

    s_eff_hz[voice] = calculate_voice_filter_cutoff_hz(voice);
    omega = cutoff_to_omega(s_eff_hz[voice], NOTE_FILTER_SAMPLE_RATE);
    init_four_pole_low_pass_filter(&s_filt[voice], omega, s_q[voice]);
}

/** Re-apply design when base/k/q/note change and filter is active. */
static void apply_voice_filter_coefficients(uint8_t voice)
{
    if (s_bypass[voice] != 0u) {
        return;
    }
    if (s_base_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ ||
        s_base_hz[voice] >= NOTE_FILTER_CUTOFF_MAX_HZ) {
        return;
    }
    calculate_voice_filter_coefficients(voice);
}

/* ---- initialize voice filters ------------------------------------------- */

void init_voice_filters(void)
{
    uint8_t i;

    for (i = 0u; i < NOTE_FILTER_VOICES; i++) {
        s_q[i] = NOTE_FILTER_Q_DEFAULT;
        s_k[i] = NOTE_FILTER_K_DEFAULT;
        s_base_hz[i] = NOTE_FILTER_CUTOFF_MAX_HZ;
        s_eff_hz[i] = NOTE_FILTER_CUTOFF_MAX_HZ;
        s_last_note_hz[i] = 0.0;
        s_bypass[i] = 1u;
        reset_four_pole_filter(&s_filt[i]);
    }
}

void reset_voice_filter(uint8_t voice)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return;
    }
    reset_four_pole_filter(&s_filt[voice]);
}

/* ---- select the filter response ----------------------------------------- */

const char *get_filter_mode_name(NoteFilter_Pass_t pass)
{
    if (pass == NOTE_FILTER_PASS_LP) {
        return "lp";
    }
    if (pass == NOTE_FILTER_PASS_HP) {
        return "hp";
    }
    if (pass == NOTE_FILTER_PASS_BP) {
        return "bp";
    }
    return "?";
}

NoteFilter_Pass_t get_voice_filter_mode(uint8_t voice)
{
    (void)voice;
    return NOTE_FILTER_PASS_LP;
}

int set_voice_filter_mode(uint8_t voice, NoteFilter_Pass_t pass)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return -1;
    }
    /* This build: LPF only; HP/BP reserved. */
    if (pass != NOTE_FILTER_PASS_LP) {
        return -2;
    }
    /* Already LP; re-apply base design if active. */
    if (s_base_hz[voice] >= NOTE_FILTER_CUTOFF_MIN_HZ &&
        s_base_hz[voice] < NOTE_FILTER_CUTOFF_MAX_HZ) {
        return set_voice_filter_cutoff_hz(voice, s_base_hz[voice]);
    }
    return 0;
}

/* ---- set and query cutoff and resonance --------------------------------- */

int set_voice_filter_cutoff_hz(uint8_t voice, double cutoff_hz)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return -1;
    }
    /* 0 = bypass alias (same as MAX); keeps console `f 0` / `f0 0` short. */
    if (cutoff_hz == 0.0) {
        cutoff_hz = NOTE_FILTER_CUTOFF_MAX_HZ;
    }
    if (cutoff_hz < NOTE_FILTER_CUTOFF_MIN_HZ || cutoff_hz > NOTE_FILTER_CUTOFF_MAX_HZ) {
        return -2;
    }

    /* q must already be valid (init_voice_filters / SetQ). Do not silent-repair. */
    if (s_q[voice] < NOTE_FILTER_Q_MIN || s_q[voice] > NOTE_FILTER_Q_MAX) {
        return -2;
    }

    s_base_hz[voice] = cutoff_hz;

    if (cutoff_hz >= NOTE_FILTER_CUTOFF_MAX_HZ) {
        s_bypass[voice] = 1u;
        s_eff_hz[voice] = NOTE_FILTER_CUTOFF_MAX_HZ;
        reset_voice_filter(voice);
        return 0;
    }

    s_bypass[voice] = 0u;
    calculate_voice_filter_coefficients(voice);
    return 0;
}

double get_voice_filter_cutoff_hz(uint8_t voice)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return 0.0;
    }
    if (s_bypass[voice] != 0u) {
        return NOTE_FILTER_CUTOFF_MAX_HZ;
    }
    if (s_eff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
        return NOTE_FILTER_CUTOFF_MAX_HZ;
    }
    return s_eff_hz[voice];
}

double get_voice_filter_base_cutoff_hz(uint8_t voice)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return 0.0;
    }
    if (s_base_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
        return NOTE_FILTER_CUTOFF_MAX_HZ;
    }
    return s_base_hz[voice];
}

int set_voice_filter_resonance(uint8_t voice, double q)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return -1;
    }
    if (q < NOTE_FILTER_Q_MIN || q > NOTE_FILTER_Q_MAX) {
        return -2;
    }

    s_q[voice] = q;
    apply_voice_filter_coefficients(voice);
    return 0;
}

double get_voice_filter_resonance(uint8_t voice)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return NOTE_FILTER_Q_DEFAULT;
    }
    if (s_q[voice] < NOTE_FILTER_Q_MIN || s_q[voice] > NOTE_FILTER_Q_MAX) {
        return NOTE_FILTER_Q_DEFAULT;
    }
    return s_q[voice];
}

/* ---- configure pitch tracking ------------------------------------------- */

int set_voice_filter_pitch_tracking(uint8_t voice, double k)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return -1;
    }
    if (k < NOTE_FILTER_K_MIN || k > NOTE_FILTER_K_MAX) {
        return -2;
    }

    s_k[voice] = k;
    apply_voice_filter_coefficients(voice);
    return 0;
}

double get_voice_filter_pitch_tracking(uint8_t voice)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return NOTE_FILTER_K_DEFAULT;
    }
    if (s_k[voice] < NOTE_FILTER_K_MIN || s_k[voice] > NOTE_FILTER_K_MAX) {
        return NOTE_FILTER_K_DEFAULT;
    }
    return s_k[voice];
}

void update_voice_filter_note_frequency(uint8_t voice, double freq_hz)
{
    if (voice >= NOTE_FILTER_VOICES) {
        return;
    }
    if (!(freq_hz > 0.0)) {
        return;
    }

    s_last_note_hz[voice] = freq_hz;
    /* Absolute mode (k==0): keep coeffs; only cache pitch for a later fk. */
    if (s_k[voice] > 0.0) {
        apply_voice_filter_coefficients(voice);
    }
}

/* ---- filter one sample -------------------------------------------------- */

int32_t filter_voice_sample(uint8_t voice, int32_t x)
{
    double in;
    double out;

    if (voice >= NOTE_FILTER_VOICES || s_bypass[voice] != 0u) {
        return x;
    }
    if (s_eff_hz[voice] < NOTE_FILTER_CUTOFF_MIN_HZ) {
        return x;
    }

    in = (double)x / 2147483647.0;
    out = filter_four_pole_sample(&s_filt[voice], in);
    return filter_sample_to_q31(out);
}
