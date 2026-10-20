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
#include "channel.h"
#include "application.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <portaudio.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

using namespace std;

/* ---- >>>>> ------------------------------------------------------------- */

static int audio_requests[2] = {-1, -1};
static int usb_port = -1;
static int rs485_port = -1;
static PaStream *stream;
static uint8_t write_index = 0;
static uint8_t read_index = 0;
static std::vector<uint8_t> usb_input;
static std::vector<uint8_t> rs485_input;
static int32_t audio_buffers[BUF_COUNT][SPB];

/* ---- set up the audio request pipe -------------------------------------- */

static void setup_select()
{
    pipe(audio_requests);
    fcntl(audio_requests[1], F_SETFL, O_NONBLOCK);
}

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
    if (port == usb_port)
    {
        int packet_mode = 1;
        ioctl(port, TIOCPKT, &packet_mode);
    }
    std::printf("%s: %s\n", label, path);
    std::fflush(stdout);
}

/* ---- read serial input -------------------------------------------------- */

static void process_serial(int port, std::vector<uint8_t> &input)
{
    unsigned char bytes[257];
    ssize_t count = read(port, bytes, sizeof bytes);
    if (count <= 0)
    {
        return;
    }
    if (port == usb_port)
    {
        if (bytes[0] != TIOCPKT_DATA)
        {
            if (bytes[0] & (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE))
            {
                input.clear();
                reset_usb_connection();
            }
            return;
        }
        input.insert(input.end(), bytes + 1, bytes + count);
        return;
    }
    input.insert(input.end(), bytes, bytes + count);
}


/* ---- consume audio and request replacement buffers ----------------------- */

static int audio_callback(const void *, void *output, unsigned long frames,
    const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags, void *)
{
    int32_t *dst = static_cast<int32_t *>(output);
    static unsigned offset = 0;
    unsigned requests = 0;
    while (frames) {
        if (read_index == write_index) {
            std::memset(dst, 0, frames * sizeof(int32_t));
            break;
        }
        unsigned const count = std::min<unsigned>(frames, SPB - offset);
        std::memcpy(dst, audio_buffers[read_index] + offset,
            count * sizeof(int32_t));
        dst += count;
        frames -= count;
        offset += count;
        if (offset == SPB) {
            offset = 0;
            read_index = (read_index + 1) % BUF_COUNT;
            ++requests;
        }
    }
    char tokens[BUF_COUNT]{};
    while (requests) {
        unsigned const count = std::min(requests, unsigned(BUF_COUNT));
        write(audio_requests[1], tokens, count);
        requests -= count;
    }
    return paContinue;
}

/* ---- configure platform scheduling for the audio generation thread ------ */

static void setup_audio_thread()
{
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    thread_time_constraint_policy_data_t policy{};
    policy.period = uint32_t(1000000ull * timebase.denom / timebase.numer);
    policy.computation = policy.period / 4;
    policy.constraint = policy.period;
    policy.preemptible = TRUE;
    mach_port_t main_thread = mach_thread_self();
    thread_policy_set(main_thread, THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), main_thread);
#endif
}

/* ---- prefill the buffers and start audio playback ------------------------ */

static void open_audio()
{
    Pa_Initialize();
    PaStreamParameters output{};
    output.device = Pa_GetDefaultOutputDevice();
    output.channelCount = 1;
    output.sampleFormat = paInt32;
    output.suggestedLatency = Pa_GetDeviceInfo(output.device)->defaultLowOutputLatency;
    Pa_OpenStream(&stream, nullptr, &output, AUDIO_SAMPLE_RATE_HZ,
        paFramesPerBufferUnspecified, paNoFlag, audio_callback, nullptr);
    for (unsigned i = 0; i < BUF_COUNT - 1; ++i)
        generate_samples(audio_buffers[i]);
    write_index = BUF_COUNT - 1;
    Pa_StartStream(stream);
    setup_audio_thread();
}

/* ---- run the simulator -------------------------------------------------- */

int main()
{
    int usb_client = -1;
    int rs485_client = -1;
    open_serial_port(usb_port, usb_client, "USB");
    open_serial_port(rs485_port, rs485_client, "RS485");
    setup_select();
    init_application();
    open_audio();
    while (true) {
        fd_set ready{};
        FD_SET(audio_requests[0], &ready);
        int highest(audio_requests[0]);

        FD_SET(usb_port, &ready);
        FD_SET(rs485_port, &ready);
        highest = std::max(highest, usb_port);
        highest = std::max(highest, rs485_port);

        int const n(select(highest + 1, &ready, nullptr, nullptr, nullptr));
        if (n < 0) continue;

        if (FD_ISSET(usb_port, &ready)) {
            process_serial(usb_port, usb_input);
            received_usb(usb_input);
            usb_input.clear();
        }
        if (FD_ISSET(rs485_port, &ready)) {
            process_serial(rs485_port, rs485_input);
            received_serial(rs485_input);
            rs485_input.clear();
        }

        if (FD_ISSET(audio_requests[0], &ready)) {
            char token;
            if (read(audio_requests[0], &token, 1) == 1) {
                generate_samples(audio_buffers[write_index]);
                write_index = (write_index + 1) % BUF_COUNT;
            }
        }
    }
}

/* ---- write RS485 bytes and report the transmitted prefix ----------------- */

void write_serial(vector<uint8_t> const &output)
{
    consume_serial_output(write(rs485_port, output.data(), output.size()));
}

/* ---- write USB bytes and report the transmitted prefix ------------------- */

void write_usb(vector<uint8_t> const &output)
{
    consume_usb_output(write(usb_port, output.data(), output.size()));
}
