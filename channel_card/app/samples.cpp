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

#include "samples.h"
#include "board.h"
#include "platform_memory.h"
#include <math.h>
#include <string.h>

/* ---- store and play sample attacks -------------------------------------- */

/* AXI signed-int8 attack heads; rate-scaled playheads (n0 pitch). */


#ifndef ATTACK_DEFAULT_ROOT_HZ
#define ATTACK_DEFAULT_ROOT_HZ 261.625565f
#endif

typedef struct {
    uint16_t wave_id;
    float phase;
    float phase_inc;
    uint8_t playing;
} AttackVoice_t;

static int8_t s_data[ATTACK_BANK_COUNT][ATTACK_BANK_LEN] CHANNEL_ATTACK_STORAGE;

static uint8_t s_loaded[ATTACK_BANK_COUNT];
static uint32_t s_len[ATTACK_BANK_COUNT];
static float s_root_hz[ATTACK_BANK_COUNT];
static AttackVoice_t s_voices[SAMPLE_VOICES];
static volatile uint8_t s_write_active;

/* ---- convert attack samples --------------------------------------------- */

static int32_t sample_int8_to_q31(int8_t s)
{
    return (int32_t)s * 16777216;
}

/* ---- initialize the attack bank ----------------------------------------- */

void init_sample_storage(void)
{
    uint16_t i;

    memset(s_loaded, 0, sizeof(s_loaded));
    memset(s_len, 0, sizeof(s_len));
    s_write_active = 0u;
    for (i = 0u; i < ATTACK_BANK_COUNT; i++) {
        s_root_hz[i] = ATTACK_DEFAULT_ROOT_HZ;
    }
    for (i = 0u; i < SAMPLE_VOICES; i++) {
        s_voices[i].playing = 0u;
        s_voices[i].phase = 0.0f;
        s_voices[i].phase_inc = 1.0f;
        s_voices[i].wave_id = 0u;
    }
}

void set_sample_storage_write_lock(uint8_t active)
{
    s_write_active = active != 0u ? 1u : 0u;
}

uint8_t is_sample_storage_write_locked(void)
{
    return s_write_active;
}

/* ---- load and commit attack data ---------------------------------------- */

int load_sample_data(uint16_t wave_id, const uint8_t *data, uint32_t nbytes)
{
    if (wave_id >= ATTACK_BANK_COUNT || data == NULL || nbytes == 0u ||
        nbytes > ATTACK_BANK_BYTES) {
        return -1;
    }
    int8_t *dst = get_sample_write_buffer(wave_id);
    if (dst == NULL) return -1;
    memcpy(dst, data, nbytes);
    memset(dst + nbytes, 0, ATTACK_BANK_BYTES - nbytes);
    return commit_sample_data(wave_id, nbytes);
}

int8_t *get_sample_write_buffer(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT) return NULL;
    return s_data[wave_id];
}

const int8_t *get_sample_data(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT) return NULL;
    return s_data[wave_id];
}

int commit_sample_data(uint16_t wave_id, uint32_t nsamp)
{
    if (wave_id >= ATTACK_BANK_COUNT || nsamp == 0u || nsamp > ATTACK_BANK_LEN) return -1;
    uint32_t saved = disable_interrupts_and_save_state();
    board_memory_barrier();
    s_len[wave_id] = nsamp;
    s_loaded[wave_id] = 1u;
    board_memory_barrier();
    restore_interrupt_state(saved);
    return 0;
}

/* ---- query loaded attacks ----------------------------------------------- */

uint32_t get_sample_length(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT || s_loaded[wave_id] == 0u) {
        return 0u;
    }
    return s_len[wave_id];
}

uint8_t is_sample_loaded(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT) {
        return 0u;
    }
    return s_loaded[wave_id];
}

uint16_t get_loaded_sample_count(void)
{
    uint16_t i;
    uint16_t n = 0u;
    for (i = 0u; i < ATTACK_BANK_COUNT; i++) {
        if (s_loaded[i] != 0u) {
            n++;
        }
    }
    return n;
}

void get_loaded_sample_mask(uint8_t *out)
{
    uint16_t i;
    if (out == NULL) {
        return;
    }
    memset(out, 0, 32);
    for (i = 0u; i < ATTACK_BANK_COUNT; i++) {
        if (s_loaded[i] != 0u) {
            out[i >> 3] = (uint8_t)(out[i >> 3] | (uint8_t)(1u << (i & 7u)));
        }
    }
}

/* ---- set the sample root frequency -------------------------------------- */

void set_sample_root_frequency_hz(uint16_t wave_id, float root_hz)
{
    if (wave_id >= ATTACK_BANK_COUNT || !(root_hz > 0.0f) || !isfinite(root_hz)) {
        return;
    }
    s_root_hz[wave_id] = root_hz;
}

float get_sample_root_frequency_hz(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT) {
        return ATTACK_DEFAULT_ROOT_HZ;
    }
    return s_root_hz[wave_id];
}

/* ---- start and stop attack playback ------------------------------------- */

int start_sample_playback(uint8_t voice, uint16_t wave_id, float phase_inc)
{
    if (voice >= SAMPLE_VOICES || wave_id >= ATTACK_BANK_COUNT || s_loaded[wave_id] == 0u) {
        return -1;
    }
    if (!(phase_inc > 0.0f) || !isfinite(phase_inc)) {
        phase_inc = 1.0f;
    }
    if (phase_inc > 16.0f) {
        phase_inc = 16.0f;
    }
    if (phase_inc < (1.0f / 16.0f)) {
        phase_inc = 1.0f / 16.0f;
    }
    s_voices[voice].wave_id = wave_id;
    s_voices[voice].phase = 0.0f;
    s_voices[voice].phase_inc = phase_inc;
    s_voices[voice].playing = 1u;
    return 0;
}

void stop_sample_playback(uint8_t voice)
{
    if (voice < SAMPLE_VOICES) {
        s_voices[voice].playing = 0u;
        s_voices[voice].phase = 0.0f;
    }
}

void stop_all_sample_playback(void)
{
    uint8_t i;
    for (i = 0u; i < SAMPLE_VOICES; i++) {
        stop_sample_playback(i);
    }
}

uint8_t is_sample_playing(uint8_t voice)
{
    if (voice >= SAMPLE_VOICES) {
        return 0u;
    }
    return s_voices[voice].playing;
}

/* ---- read attack samples ------------------------------------------------ */

int32_t render_next_stored_sample(uint8_t voice)
{
    AttackVoice_t *v;
    uint16_t wid;
    float ph;
    uint32_t i0;
    uint32_t i1;
    uint32_t n;
    float frac;
    double a;
    double b;
    double y;

    if (voice >= SAMPLE_VOICES) {
        return 0;
    }
    v = &s_voices[voice];
    if (v->playing == 0u) {
        return 0;
    }

    wid = v->wave_id;
    n = s_len[wid];
    if (n == 0u) {
        n = ATTACK_BANK_LEN;
    }
    ph = v->phase;
    if (ph >= (float)n) {
        v->playing = 0u;
        v->phase = 0.0f;
        return sample_int8_to_q31(s_data[wid][n - 1u]);
    }

    i0 = (uint32_t)ph;
    if (i0 >= n) {
        v->playing = 0u;
        return sample_int8_to_q31(s_data[wid][n - 1u]);
    }
    i1 = i0 + 1u;
    frac = ph - (float)i0;
    a = (double)sample_int8_to_q31(s_data[wid][i0]);
    if (i1 < n) {
        b = (double)sample_int8_to_q31(s_data[wid][i1]);
        y = a + (b - a) * (double)frac;
    } else {
        y = a;
    }

    ph += v->phase_inc;
    v->phase = ph;
    if (ph >= (float)n) {
        v->playing = 0u;
    }

    return (int32_t)lround(y);
}

int32_t get_sample_value_at(uint16_t wave_id, uint32_t index)
{
    if (wave_id >= ATTACK_BANK_COUNT || index >= s_len[wave_id] || s_loaded[wave_id] == 0u) {
        return 0;
    }
    return sample_int8_to_q31(s_data[wave_id][index]);
}
