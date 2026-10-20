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

/* ---- query audio diagnostics -------------------------------------------- */

static const AudioOutputDiagnostics *s_output_diagnostics;

/* ---- bind platform audio diagnostics ----------------------------------- */

void set_audio_output_diagnostics(const AudioOutputDiagnostics *diagnostics)
{
    s_output_diagnostics = diagnostics;
}

/* ---- read platform refill overlap count -------------------------------- */

uint32_t get_audio_refill_overlap_count(void)
{
    return s_output_diagnostics ? s_output_diagnostics->refill_overlap_count() : 0u;
}

/* ---- read dropped stream block count ----------------------------------- */

uint32_t get_audio_stream_drop_count(void)
{
    return get_stream_dropped_block_count();
}

/* ---- clear audio and stream statistics --------------------------------- */

void clear_audio_stream_statistics(void)
{
    clear_stream_statistics();
    clear_voice_underrun_count();
    if (s_output_diagnostics)
    {
        s_output_diagnostics->clear_statistics();
    }
}

/* ---- read maximum stream occupancy ------------------------------------- */

uint32_t get_audio_stream_max_buffered_samples(void)
{
    return get_max_stream_buffered_samples();
}
