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

#include "audio.h"
#include "voice.h"
#include "stream.h"

/* ---- render sample-clocked application intervals ------------------------- */

/* Cold-start state, independent of transport resets and voice clear commands.
 * Only the single audio consumer owns this cursor. */
static uint32_t interval_position;

void render_audio_samples(int32_t *samples, uint32_t sample_count)
{
    while (sample_count) {
        if (interval_position == 0u) begin_voice_audio_block();
        uint32_t count = AUDIO_PROCESSING_INTERVAL_SAMPLES - interval_position;
        if (count > sample_count) count = sample_count;
        for (uint32_t i = 0; i < count; ++i) *samples++ = mix_next_audio_sample();
        interval_position += count;
        sample_count -= count;
        if (interval_position == AUDIO_PROCESSING_INTERVAL_SAMPLES) {
            end_voice_audio_block();
            interval_position = 0u;
        }
    }
}

/* ---- query audio diagnostics -------------------------------------------- */

static const AudioOutputDiagnostics *s_output_diagnostics;

void set_audio_output_diagnostics(const AudioOutputDiagnostics *diagnostics)
{
    s_output_diagnostics = diagnostics;
}

uint32_t get_audio_refill_overlap_count(void)
{
    return s_output_diagnostics ? s_output_diagnostics->refill_overlap_count() : 0u;
}

uint32_t get_audio_stream_drop_count(void)
{
    return get_stream_dropped_block_count();
}

void clear_audio_stream_statistics(void)
{
    clear_stream_statistics();
    clear_voice_underrun_count();
    if (s_output_diagnostics) s_output_diagnostics->clear_statistics();
}

uint32_t get_audio_stream_max_buffered_samples(void)
{
    return get_max_stream_buffered_samples();
}
