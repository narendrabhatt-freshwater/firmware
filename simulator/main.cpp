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

#include "hardware.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <portaudio.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

/* ---- pipes ------------------------------------------------------------- */

static int audio_requests[2] = {-1, -1};
static int usb_port = -1;
static int rs485_port = -1;
static PaStream *stream;
static uint8_t write_index;

/* ---- set up the audio request pipe -------------------------------------- */

static void setup_select()
{
    pipe(audio_requests);
    fcntl(audio_requests[1], F_SETFL, O_NONBLOCK);
    write_index = 0;
}

/* ---- serial input ------------------------------------------------------ */

static std::vector<uint8_t> usb_input;
static std::vector<uint8_t> rs485_input;

/* ---- open a serial port ------------------------------------------------- */

static void open_serial_port(int &port, int &client, const char *label)
{
    port = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    grantpt(port);
    unlockpt(port);
    const char *path = ptsname(port);
    client = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    termios mode;
    tcgetattr(client, &mode);
    cfmakeraw(&mode);
    mode.c_cflag |= CLOCAL | CREAD;
    tcsetattr(client, TCSANOW, &mode);
    std::printf("%s: %s\n", label, path);
    std::fflush(stdout);
}

/* ---- read serial input -------------------------------------------------- */

static void process_serial(int port, std::vector<uint8_t> &input)
{
    unsigned char bytes[256];
    ssize_t count = read(port, bytes, sizeof bytes);
    if (count > 0) input.insert(input.end(), bytes, bytes + count);
}

/* ---- wait for serial input or one audio token --------------------------- */

static void wait_select()
{
    fd_set ready{};
    FD_SET(audio_requests[0], &ready);
    int highest = audio_requests[0];
    for (int fd : {usb_port, rs485_port})
    {
        if (fd < 0) continue;
        FD_SET(fd, &ready);
        if (fd > highest) highest = fd;
    }
    select(highest + 1, &ready, nullptr, nullptr, nullptr);
    if (FD_ISSET(audio_requests[0], &ready))
    {
        char token;
        read(audio_requests[0], &token, 1);
    }
}

static int32_t audio_buffers[BUF_COUNT][AUDIO_BUFFER_FRAMES];

/* ---- generate a sine wave buffer ---------------------------------------- */

static void generate_audio_samples()
{
    static double phase = 0;
    constexpr double tau = 6.28318530717958647692;
    for (auto &sample : audio_buffers[write_index])
    {
        sample = static_cast<int32_t>(std::sin(phase) * (INT32_MAX / 4.0));
        phase += tau * 440.0 / AUDIO_SAMPLE_RATE_HZ;
        if (phase >= tau) phase -= tau;
    }
    write_index = (write_index + 1) % BUF_COUNT;
    if (write_index == 0) std::printf("Audio refill complete\n");
}

/* ---- play the audio buffers and request refills -------------------------- */

static int audio_callback(const void *, void *output, unsigned long frames,
    const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags, void *)
{
    std::printf("Audio callback requested %lu frames\n", frames);
    char tokens[BUF_COUNT]{};
    std::memcpy(output, audio_buffers, sizeof audio_buffers);
    write(audio_requests[1], tokens, sizeof tokens);
    return paContinue;
}

/* ---- prefill the buffers and start audio playback ------------------------ */

static void open_audio()
{
    Pa_Initialize();
    Pa_OpenDefaultStream(&stream, 0, 1, paInt32, AUDIO_SAMPLE_RATE_HZ,
        BUF_COUNT * AUDIO_BUFFER_FRAMES, audio_callback, nullptr);
    for (unsigned i = 0; i < BUF_COUNT; ++i) generate_audio_samples();
    Pa_StartStream(stream);
}

/* ---- run the simulator -------------------------------------------------- */

int main()
{
    int usb_client = -1;
    int rs485_client = -1;
    open_serial_port(usb_port, usb_client, "USB");
    open_serial_port(rs485_port, rs485_client, "RS485");
    setup_select();
    open_audio();
    while (true)
    {
        wait_select();
        process_serial(rs485_port, rs485_input);
        process_serial(usb_port, usb_input);
        generate_audio_samples();
    }
}
