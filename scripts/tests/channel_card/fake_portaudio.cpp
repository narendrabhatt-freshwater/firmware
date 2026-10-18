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

/* Test-only PortAudio replacement for isolated PTY/protocol checks. The
 * production simulator always links the real library and opens an output. */
#include <portaudio.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

static PaStreamCallback *callback;
static void *context;
static std::atomic<bool> running{false};
static std::thread worker;
static const PaStreamInfo info{1, 0, 128.0 / 48000, 48000};

PaError Pa_Initialize() { return paNoError; }
PaError Pa_Terminate() { return paNoError; }
PaDeviceIndex Pa_GetDefaultOutputDevice() { return 0; }
const char *Pa_GetErrorText(PaError) { return "test backend error"; }
const PaStreamInfo *Pa_GetStreamInfo(PaStream *) { return &info; }
PaError Pa_OpenDefaultStream(PaStream **stream, int input, int output,
    PaSampleFormat format, double rate, unsigned long frames,
    PaStreamCallback *cb, void *user)
{
    assert(input == 0 && output == 2 && format == paInt32 && rate == 48000 && frames == 128);
    callback = cb;
    context = user;
    *stream = reinterpret_cast<PaStream *>(1);
    return paNoError;
}
PaError Pa_StartStream(PaStream *)
{
    running = true;
    // Explicit fault injection for software-clock diagnostics only.
    const char *stall_text = getenv("CHANNEL_TEST_AUDIO_STALL_MS");
    unsigned stall_ms = stall_text ? (unsigned)strtoul(stall_text, nullptr, 10) : 0;
    worker = std::thread([stall_ms] {
        unsigned callbacks = 0;
        auto next = std::chrono::steady_clock::now();
        int32_t output[256];
        PaStreamCallbackTimeInfo time{};
        while (running) {
            if (++callbacks == 375 && stall_ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
            if (callback(nullptr, output, 128, &time, 0, context) != paContinue) {
                running = false;
                break;
            }
            next += std::chrono::nanoseconds(128000000000ull / 48000);
            std::this_thread::sleep_until(next);
        }
    });
    return paNoError;
}
PaError Pa_IsStreamActive(PaStream *) { return running ? 1 : 0; }
PaError Pa_StopStream(PaStream *)
{
    running = false;
    if (worker.joinable()) worker.join();
    return paNoError;
}
PaError Pa_CloseStream(PaStream *stream) { return Pa_StopStream(stream); }
