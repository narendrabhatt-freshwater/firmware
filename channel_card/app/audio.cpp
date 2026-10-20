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

#include "audio_buffers.h"
#include "channel.h"
#include "audio_output.h"

AudioBuffers audio_output CHANNEL_AUDIO_STORAGE;
AudioBuffers control_voltage_output CHANNEL_AUDIO_STORAGE;

static_assert(AUDIO_BUFFER_FRAMES == AUDIO_PROCESSING_INTERVAL_SAMPLES,
              "Application buffers hold exactly 48 samples");

/* ---- render shared audio into the STM32 output rings -------------------- */

void generate_audio_samples(void)
{
    if (audio_output.write_index !=
        (__atomic_load_n(&audio_output.read_index, __ATOMIC_ACQUIRE) + BUF_COUNT) % (2u * BUF_COUNT))
    {
        int32_t *samples = audio_output.samples[audio_output.write_index % BUF_COUNT];
        int32_t mono[AUDIO_PROCESSING_INTERVAL_SAMPLES];
        generate_samples(mono);
        for (uint32_t i = 0; i < AUDIO_BUFFER_FRAMES; ++i)
        {
            samples[i * AUDIO_BUFFER_CHANNELS] = mono[i];
        }
        render_control_voltage_slot(samples, AUDIO_BUFFER_FRAMES, 1, 1);
        __atomic_store_n(&audio_output.write_index,
                         (audio_output.write_index + 1u) % (2u * BUF_COUNT), __ATOMIC_RELEASE);
    }
    if (control_voltage_output.write_index !=
        (__atomic_load_n(&control_voltage_output.read_index, __ATOMIC_ACQUIRE) + BUF_COUNT) % (2u * BUF_COUNT))
    {
        render_control_voltage_channels(
            control_voltage_output.samples[control_voltage_output.write_index % BUF_COUNT], AUDIO_BUFFER_FRAMES,
            2, 3);
        __atomic_store_n(&control_voltage_output.write_index,
                         (control_voltage_output.write_index + 1u) % (2u * BUF_COUNT), __ATOMIC_RELEASE);
    }
}
