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
#include "platform.h"
#include "platform_memory.h"
#include <math.h>
#include <string.h>

/* ---- store sample attacks -------------------------------------- */

/* Signed-int8 attack storage. voice.cpp owns all playback state. */


#ifndef ATTACK_DEFAULT_ROOT_HZ
#define ATTACK_DEFAULT_ROOT_HZ 261.625565f
#endif

static int8_t s_data[ATTACK_BANK_COUNT][ATTACK_BANK_LEN] CHANNEL_ATTACK_STORAGE;

static uint32_t s_len[ATTACK_BANK_COUNT];
static float s_root_hz[ATTACK_BANK_COUNT];
static volatile uint8_t s_write_active;

/* ---- initialize the attack bank ----------------------------------------- */

void init_sample_storage(void)
{
    uint16_t i;

    memset(s_len, 0, sizeof(s_len));
    s_write_active = 0u;
    for (i = 0u; i < ATTACK_BANK_COUNT; i++) {
        s_root_hz[i] = ATTACK_DEFAULT_ROOT_HZ;
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
    board_memory_barrier();
    restore_interrupt_state(saved);
    return 0;
}

/* ---- query loaded attacks ----------------------------------------------- */

uint32_t get_sample_length(uint16_t wave_id)
{
    if (wave_id >= ATTACK_BANK_COUNT) {
        return 0u;
    }
    return s_len[wave_id];
}

uint16_t get_loaded_sample_count(void)
{
    uint16_t i;
    uint16_t n = 0u;
    for (i = 0u; i < ATTACK_BANK_COUNT; i++) {
        if (s_len[i] != 0u) {
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
        if (s_len[i] != 0u) {
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
