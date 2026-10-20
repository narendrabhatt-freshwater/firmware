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

#ifndef CHANNEL_APP_AUDIO_H
#define CHANNEL_APP_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- set the audio sample rate ------------------------------------------ */

/* All platforms and filter coefficients use the same sample rate. */
#define AUDIO_SAMPLE_RATE_HZ 48000u

/* ---- render audio samples ----------------------------------------------- */

/* Each application call generates one millisecond of mono audio. */
#define AUDIO_PROCESSING_INTERVAL_SAMPLES (AUDIO_SAMPLE_RATE_HZ / 1000u)

/* ---- query audio diagnostics -------------------------------------------- */

/* Optional output-driver diagnostics. No output device means zero overlaps.
 * Bind a complete, static-lifetime table before playback begins. */
typedef struct {
    uint32_t (*refill_overlap_count)(void);
    void (*clear_statistics)(void);
} AudioOutputDiagnostics;
void set_audio_output_diagnostics(const AudioOutputDiagnostics *diagnostics);

uint32_t get_audio_stream_drop_count(void);
void clear_audio_stream_statistics(void);
uint32_t get_audio_stream_max_buffered_samples(void);
uint32_t get_audio_refill_overlap_count(void);

#ifdef __cplusplus
}
#endif

#endif
