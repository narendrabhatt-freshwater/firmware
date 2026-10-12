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

#include "audio_device.h"
#include "audio.h"
#include <portaudio.h>
#include <stdint.h>
#include <stdio.h>

/* ---- own the PortAudio stream ------------------------------------------- */

static PaStream *output_stream;
static bool initialized, started;
static uint32_t callback_errors;
static constexpr uint32_t invalid_request = 0x80000000u;
static constexpr unsigned output_channels = 2;
/* Match MAS's requested callback size. The renderer still accepts the actual
 * frame count supplied by PortAudio and retains its own 48-sample cadence. */
static constexpr unsigned callback_frames = 128;
static_assert(__atomic_always_lock_free(sizeof(callback_errors), nullptr),
              "Callback error publication must not acquire a lock");

static bool check_audio_status(PaError error, const char *operation)
{
    if (error == paNoError) return true;
    fprintf(stderr, "sim: %s failed: %s (%d)\n", operation, Pa_GetErrorText(error), error);
    return false;
}

/* ---- fill the buffer requested by the sound device ---------------------- */

static int audio_callback(const void *, void *output, unsigned long frames,
                          const PaStreamCallbackTimeInfo *,
                          PaStreamCallbackFlags flags, void *)
{
    uint32_t error = (uint32_t)(flags & (paOutputUnderflow | paOutputOverflow));
    if (!output || frames > UINT32_MAX / output_channels) error |= invalid_request;
    if (error) {
        __atomic_fetch_or(&callback_errors, error, __ATOMIC_RELEASE);
        return paAbort;
    }
    auto *samples = static_cast<int32_t *>(output);
    render_audio_samples(samples, (uint32_t)frames);
    /* Expand in place, backwards, so both speakers receive the mono mix.
     * No staging queue, allocation, or format scaling is needed. */
    for (unsigned long i = frames; i-- > 0;) {
        int32_t sample = samples[i];
        samples[2 * i] = samples[2 * i + 1] = sample;
    }
    return paContinue;
}

/* ---- open and monitor the default output -------------------------------- */

bool start_audio_device(void)
{
    __atomic_store_n(&callback_errors, 0u, __ATOMIC_RELAXED);
    if (!check_audio_status(Pa_Initialize(), "initializing PortAudio")) return false;
    initialized = true;
    if (Pa_GetDefaultOutputDevice() == paNoDevice) {
        fprintf(stderr, "sim: no default audio output device\n");
        stop_audio_device();
        return false;
    }
    if (!check_audio_status(Pa_OpenDefaultStream(&output_stream, 0, output_channels,
            paInt32, AUDIO_SAMPLE_RATE_HZ, callback_frames, audio_callback, nullptr),
            "opening audio output")) {
        stop_audio_device();
        return false;
    }
    if (!check_audio_status(Pa_StartStream(output_stream), "starting audio output")) {
        stop_audio_device();
        return false;
    }
    started = true;
    const PaStreamInfo *info = Pa_GetStreamInfo(output_stream);
    if (!info) {
        fprintf(stderr, "sim: cannot read audio stream information\n");
        stop_audio_device();
        return false;
    }
    fprintf(stderr, "sim: PortAudio default output, %.0f Hz, signed 32-bit, "
            "mono to stereo; %u-frame callback, %.2f ms reported output latency\n",
            info->sampleRate, callback_frames, info->outputLatency * 1000);
    return true;
}

bool process_audio_device(void)
{
    uint32_t errors = __atomic_load_n(&callback_errors, __ATOMIC_ACQUIRE);
    if (errors) {
        if (errors & paOutputUnderflow) fprintf(stderr, "sim: audio output underflow\n");
        if (errors & paOutputOverflow) fprintf(stderr, "sim: audio output overflow\n");
        if (errors & invalid_request) fprintf(stderr, "sim: invalid audio callback buffer\n");
        return false;
    }
    PaError active = Pa_IsStreamActive(output_stream);
    if (active == 1) return true;
    if (active < 0) return check_audio_status(active, "checking audio output");
    fprintf(stderr, "sim: audio output stopped unexpectedly\n");
    return false;
}

bool stop_audio_device(void)
{
    bool ok = true;
    if (output_stream) {
        if (started) ok = check_audio_status(Pa_StopStream(output_stream), "stopping audio output");
        if (!check_audio_status(Pa_CloseStream(output_stream), "closing audio output")) ok = false;
        output_stream = nullptr;
        started = false;
    }
    if (initialized) {
        if (!check_audio_status(Pa_Terminate(), "terminating PortAudio")) ok = false;
        initialized = false;
    }
    return ok;
}
