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

/* Observe the real PortAudio callback using preallocated test memory.
 * File output and application diagnostics are read only after callbacks stop. */
#include <portaudio.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serialdevices.h"
#include "voice.h"
#include "stream.h"

static pthread_t main_thread;
static PaStreamCallback *render_callback;
static constexpr unsigned capacity_frames = 48000 * 10;
struct Block { uint64_t timestamp; uint32_t frames; };
static Block blocks[capacity_frames];
static int32_t capture[capacity_frames * 2];
static unsigned block_count, frame_count;
static uint64_t maximum_callback_ns;
static double first_dac_time, last_dac_time;

static int observe_callback(const void *input, void *output, unsigned long frames,
    const PaStreamCallbackTimeInfo *time, PaStreamCallbackFlags flags, void *context)
{
    assert(!pthread_equal(main_thread, pthread_self()));
    uint64_t begin = get_simulator_time_ns();
    int result = render_callback(input, output, frames, time, flags, context);
    uint64_t end = get_simulator_time_ns();
    if (result != paContinue) return result;
    if (end - begin > maximum_callback_ns) maximum_callback_ns = end - begin;
    assert(frame_count + frames <= capacity_frames && block_count < capacity_frames);
    blocks[block_count++] = {end, (uint32_t)frames};
    memcpy(capture + frame_count * 2, output, frames * 8);
    frame_count += (unsigned)frames;
    if (block_count == 1) first_dac_time = time->outputBufferDacTime;
    last_dac_time = time->outputBufferDacTime + frames / 48000.0;
    return paContinue;
}
static PaError observe_open(PaStream **stream, int input, int output,
    PaSampleFormat format, double rate, unsigned long frames,
    PaStreamCallback *callback, void *context)
{
    render_callback = callback;
    return Pa_OpenDefaultStream(stream, input, output, format, rate, frames, observe_callback, context);
}
#define Pa_OpenDefaultStream observe_open
#include "audio_device.cpp"
#undef Pa_OpenDefaultStream

#define main simulator_main
#include "pty_main.cpp"
#undef main

int main(int argc, char **argv)
{
    main_thread = pthread_self();
    int result = simulator_main(argc, argv);
    if (result) return result;
    FILE *file = fopen(getenv("CHANNEL_AUDIO_CAPTURE"), "wb");
    assert(file);
    unsigned offset = 0;
    for (unsigned i = 0; i < block_count; ++i) {
        uint32_t bytes = blocks[i].frames * 8;
        assert(fwrite(&blocks[i].timestamp, 8, 1, file) == 1);
        assert(fwrite(&bytes, 4, 1, file) == 1);
        assert(fwrite(capture + offset * 2, 1, bytes, file) == bytes);
        offset += blocks[i].frames;
    }
    assert(fclose(file) == 0);
    fprintf(stderr, "audio-test: device requested %u frames; %u callbacks; DAC timeline %.3f s; "
            "maximum callback %.3f ms\n", frame_count, block_count,
            last_dac_time - first_dac_time, maximum_callback_ns / 1e6);
    fprintf(stderr, "audio-test: voice underruns %u; dropped BODY blocks %u\n",
            get_voice_underrun_count(), get_stream_dropped_block_count());
    assert(block_count > 0 && last_dac_time > first_dac_time);
    return 0;
}
