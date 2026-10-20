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

#ifndef CHANNEL_APP_SAMPLES_H
#define CHANNEL_APP_SAMPLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- store sample attacks ----------------------------------------------- */

/* AXI signed-int8 attack heads (256 × ATTACK_BANK_LEN).
 * Eight voices assign loaded sample IDs 0..247. IDs 248..255 are reserved
 * for script oscillators. Sample heads play before USB stream_ring sustain. */

/** SAMPLE voices n0..n7. */
#define SAMPLE_VOICES 8u

/** Stored heads (MIDI-sized bank). Independent of SAMPLE_VOICES. */
#define ATTACK_BANK_COUNT 256u
#define ATTACK_BANK_WAVETABLE_COUNT 8u
#define ATTACK_BANK_WAVETABLE_FIRST (ATTACK_BANK_COUNT - ATTACK_BANK_WAVETABLE_COUNT)
#define ATTACK_BANK_SAMPLE_COUNT ATTACK_BANK_WAVETABLE_FIRST
/** ~10.7 ms @ 48 kHz. 256 × 512 int8 = 128 KB AXI. */
#define ATTACK_BANK_LEN 512u
#define ATTACK_BANK_BYTES ATTACK_BANK_LEN
/** Source-index overlap of attack tail with body head. */
#define SAMPLE_CROSSFADE_LEN 32u
/** Join index when the head is committed at full length. */
#define SAMPLE_BODY_ORIGIN (ATTACK_BANK_LEN - SAMPLE_CROSSFADE_LEN)

/* ---- initialize and load sample storage --------------------------------- */

void init_sample_storage(void);

/**
 * @brief Replace one head with signed int8 bytes (1..ATTACK_BANK_BYTES).
 * @retval 0 ok
 * @retval -1 bad id / size / null
 */
int load_sample_data(uint16_t wave_id, const uint8_t *data, uint32_t nbytes);

/** Direct live write pointer for one serialized main-loop upload (ATTACK_BANK_LEN int8). */
int8_t *get_sample_write_buffer(uint16_t wave_id);

/** Prevent note admission during guarded oscillator wavetable uploads. */
void set_sample_storage_write_lock(uint8_t active);
uint8_t is_sample_storage_write_locked(void);

/** Main-loop-only update of completed head length and loaded state. */
int commit_sample_data(uint16_t wave_id, uint32_t nsamp);

/* ---- inspect stored samples --------------------------------------------- */

uint8_t is_sample_loaded(uint16_t wave_id);

/** Committed samples (0 if empty). Join is at this index, not ATTACK_BANK_LEN. */
uint32_t get_sample_length(uint16_t wave_id);

/** Loaded head count (0..256). */
uint16_t get_loaded_sample_count(void);

/** 256-bit mask, bit i = head i loaded. out must be 32 bytes. */
void get_loaded_sample_mask(uint8_t *out);

/* ---- configure sample tuning -------------------------------------------- */

void set_sample_root_frequency_hz(uint16_t wave_id, float root_hz);
float get_sample_root_frequency_hz(uint16_t wave_id);

/* ---- play stored samples ------------------------------------------------ */

/**
 * @brief Start attack playhead for voice (0..7) from wave_id at phase_inc
 *        (samples of table per output sample). phase_inc from note_hz/root.
 */
int start_sample_playback(uint8_t voice, uint16_t wave_id, float phase_inc);

void stop_sample_playback(uint8_t voice);
void stop_all_sample_playback(void);

/** Non-zero while attack samples remain for this voice. */
uint8_t is_sample_playing(uint8_t voice);

/**
 * @brief Next Q31 sample for voice (rate-scaled). Last table sample
 *        when the head ends (not 0).
 */
int32_t render_next_stored_sample(uint8_t voice);

/* ---- read stored sample data -------------------------------------------- */

/** Table sample as Q31; 0 if id/index invalid or unloaded. */
int32_t get_sample_value_at(uint16_t wave_id, uint32_t index);

/** Contiguous signed-int8 head (ATTACK_BANK_LEN). NULL if id invalid. */
const int8_t *get_sample_data(uint16_t wave_id);

#ifdef __cplusplus
}
#endif

#endif
