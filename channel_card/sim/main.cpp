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
#include "channel.h"
#include "serialdevices.h"
#include "usb.h"
#include "transport.h"
#include <signal.h>
#include <stdio.h>

/* ---- handle termination signals ----------------------------------------- */

static volatile sig_atomic_t stopped;

static void stop_simulator(int)
{
    stopped = 1;
}

/* ---- run the simulator -------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "Usage: %s\n", argv[0]);
        return 1;
    }
    if (!init_simulator_timing()) return 1;
    signal(SIGINT, stop_simulator);
    signal(SIGTERM, stop_simulator);
    signal(SIGPIPE, SIG_IGN);

    init_usb_protocol();
    init_rs485_receiver();
    init_channel_card();
    if (!start_audio_device()) return 1;
    /* Publish ports only after the sound device is ready for playback. */
    if (!setup_serial_devices()) {
        stop_audio_device();
        return 1;
    }
    fprintf(stderr, "sim: USB %s, RS485 %s (Ctrl+C to stop)\n", SIM_USB_PATH, SIM_RS485_PATH);

    /* ---- service commands while PortAudio requests samples -------------- */

    int result = 0;
    while (!stopped) {
        /* This wait discovers PTY connections; it never generates audio. */
        if (!process_serial_io(get_simulator_time_ns() + 1000000u)) {
            result = 1;
            break;
        }
        process_usb_messages();
        process_rs485_commands();
        process_usb_messages();
        if (!process_audio_device()) {
            result = 1;
            break;
        }
    }

    /* ---- stop callbacks before closing their application context -------- */

    if (!stop_audio_device()) result = 1;
    close_serial_devices();
    return result;
}
