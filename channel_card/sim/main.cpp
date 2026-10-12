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
#include "channel.h"
#include "serialdevices.h"
#include "usb.h"
#include "transport.h"
#include <signal.h>
#include <stdio.h>

#ifndef SIM_AUDIO_SAMPLES
#define SIM_AUDIO_SAMPLES 48u
#endif

/* ---- handle termination signals ----------------------------------------- */

static volatile sig_atomic_t stopped;

static void stop_simulator(int)
{
    stopped = 1;
}

/* Print one complete block per second. */
static void print_audio_block(const int32_t *block, uint32_t sample_count)
{
    static uint64_t last_print = 0;
    uint64_t now = get_simulator_time_ns();
    if (now - last_print < 1000000000ULL) return;

    fprintf(stderr, "sim: block:");
    for (uint32_t i = 0; i < sample_count; ++i) {
        fprintf(stderr, " %ld", (long)block[i]);
    }
    fprintf(stderr, "\n");
    last_print = now;
}

/* ---- run the simulator -------------------------------------------------- */

int main(void)
{
    /* ---- initialize devices and application ----------------------------- */

    if (!init_simulator_timing() || !setup_serial_devices()) return 1;
    signal(SIGINT, stop_simulator);
    signal(SIGTERM, stop_simulator);
    signal(SIGPIPE, SIG_IGN);

    init_usb_protocol();
    init_rs485_receiver();
    init_channel_card();
    fprintf(stderr, "sim: USB %s, RS485 %s (Ctrl+C to stop)\n", SIM_USB_PATH, SIM_RS485_PATH);

    /* ---- service serial ports and render audio -------------------------- */

    constexpr uint32_t sample_count = SIM_AUDIO_SAMPLES;
    static_assert(sample_count > 0, "Audio requests must advance time");
    uint64_t next_audio = start_audio_clock(sample_count, AUDIO_SAMPLE_RATE_HZ);
    int result = 0;
    while (!stopped) {
        if (!process_serial_io(next_audio)) {
            result = 1;
            break;
        }
        process_usb_messages();
        process_rs485_commands();
        process_usb_messages(); /* Drain USB after a console reply. */
        if (get_simulator_time_ns() < next_audio) continue;

        int32_t block[sample_count];
        render_audio_samples(block, sample_count);
        next_audio = schedule_next_audio_block();
        print_audio_block(block, sample_count);
    }

    /* ---- close devices -------------------------------------------------- */

    close_serial_devices();
    return result;
}
