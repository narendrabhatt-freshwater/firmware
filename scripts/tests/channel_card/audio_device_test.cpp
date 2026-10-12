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

/* Drive the production PortAudio callback and exercise lifecycle failures.
 * These function doubles never open a sound device. */
#include <portaudio.h>
#include <assert.h>
#include <stdint.h>
#include <vector>

static unsigned step, fail_step, closes, stops, terminations;
static bool no_device, no_info;
static int active = 1;
static PaStreamCallback *callback;
static void *context;
static unsigned renders;
static int32_t render_value;
static const PaStreamInfo info{1, 0, 0.01, 48000};
static PaError result() { return ++step == fail_step ? paInternalError : paNoError; }

PaError Pa_Initialize() { return result(); }
PaError Pa_Terminate() { ++terminations; return result(); }
PaDeviceIndex Pa_GetDefaultOutputDevice() { return no_device ? paNoDevice : 0; }
const char *Pa_GetErrorText(PaError) { return "injected test error"; }
const PaStreamInfo *Pa_GetStreamInfo(PaStream *) { return no_info ? nullptr : &info; }
PaError Pa_OpenDefaultStream(PaStream **stream, int input, int output,
    PaSampleFormat format, double rate, unsigned long frames,
    PaStreamCallback *cb, void *user)
{
    assert(input == 0 && output == 2 && format == paInt32 && rate == 48000 && frames == 128);
    PaError error = result();
    if (!error) {
        *stream = reinterpret_cast<PaStream *>(1);
        callback = cb;
        context = user;
    }
    return error;
}
PaError Pa_StartStream(PaStream *) { return result(); }
PaError Pa_IsStreamActive(PaStream *) { return active; }
PaError Pa_StopStream(PaStream *) { ++stops; return result(); }
PaError Pa_CloseStream(PaStream *) { ++closes; return result(); }

static int32_t expected(unsigned i)
{
    const int32_t values[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    return values[i % 5] ^ render_value;
}
extern "C" void render_audio_samples(int32_t *out, uint32_t count)
{
    ++renders;
    for (uint32_t i = 0; i < count; ++i) out[i] = expected(i);
}
#include "audio_device.cpp"

static void reset()
{
    assert(!output_stream && !initialized);
    step = fail_step = closes = stops = terminations = renders = 0;
    active = 1;
    render_value = 0;
    no_device = no_info = false;
}

static void render(unsigned frames)
{
    std::vector<int32_t> output(2 * frames + 2, 0x12345678);
    assert(callback(nullptr, output.data() + 1, frames, nullptr, 0, context) == paContinue);
    for (unsigned i = 0; i < frames; ++i)
        assert(output[1 + 2 * i] == expected(i) && output[2 + 2 * i] == expected(i));
    assert(output.front() == 0x12345678 && output.back() == 0x12345678);
}

int main()
{
    reset(); assert(start_audio_device());
    assert(renders == 0); // Startup itself doesn't render ahead.
    for (unsigned frames : {1u, 7u, 48u, 128u, 511u, 512u, 1024u}) render(frames);
    assert(process_audio_device());
    unsigned before = renders;
    assert(process_audio_device() && renders == before); // Main loop cannot generate samples.

    assert(stop_audio_device() && closes == 1 && stops == 1 && terminations == 1);
    assert(stop_audio_device() && terminations == 1);

    for (unsigned failure : {1u, 2u, 3u}) {
        reset(); fail_step = failure;
        assert(!start_audio_device() && !output_stream && !initialized);
        assert(terminations == (failure > 1 ? 1u : 0u));
        assert(closes == (failure > 2 ? 1u : 0u));
    }
    reset(); no_device = true;
    assert(!start_audio_device() && terminations == 1 && closes == 0);
    reset(); no_info = true;
    assert(!start_audio_device() && stops == 1 && closes == 1 && terminations == 1);
    for (auto flags : {paOutputUnderflow, paOutputOverflow}) {
        reset(); assert(start_audio_device());
        int32_t output[256]{};
        assert(callback(nullptr, output, 128, nullptr, flags, context) == paAbort);
        assert(!process_audio_device() && renders == 0);
        assert(stop_audio_device());
    }
    reset(); assert(start_audio_device());
    assert(callback(nullptr, nullptr, 128, nullptr, 0, context) == paAbort);
    assert(!process_audio_device() && stop_audio_device());
    for (int state : {0, (int)paInternalError}) {
        reset(); assert(start_audio_device()); active = state;
        assert(!process_audio_device() && stop_audio_device());
    }
    for (unsigned failure : {1u, 2u, 3u}) {
        reset(); assert(start_audio_device()); fail_step = step + failure;
        assert(!stop_audio_device());
        assert(closes == 1 && terminations == 1 && !output_stream && !initialized);
    }
    return 0;
}
