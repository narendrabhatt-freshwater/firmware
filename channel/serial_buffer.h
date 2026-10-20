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

#ifndef CHANNEL_APP_SERIAL_BUFFER_H
#define CHANNEL_APP_SERIAL_BUFFER_H

#include <stddef.h>
#include <stdint.h>

// Application-owned byte storage, shared with each platform's main-loop I/O.
// Hardware drivers keep their interrupt-owned queues private.
struct SerialBuffer {
    uint8_t input[8192];
    size_t input_size = 0;
    uint8_t output[2048];
    size_t output_size = 0;
    // Updated by the platform's main-loop I/O service.
    bool connected = true;
    bool faulted = false;
    uint32_t generation = 1;
    uint32_t dropped = 0;
    bool clear_input = false;
    bool transmit_failed = false;
    void (*format_status)(char *, uint32_t) = nullptr;
    void (*process)(void) = nullptr; // Bound by the application.
};

extern SerialBuffer usb_serial_buffer;
extern SerialBuffer rs485_serial_buffer;

// Record how many bytes were read into the free input space.
void serial_bytes_received(SerialBuffer &buffer, size_t bytes_received);
size_t read_serial_input(SerialBuffer &buffer, uint8_t *destination, size_t size);
// Copy all bytes or return false without accepting any. Caller retains data
// on rejection. Queued bytes remain pending until the backend writes them.
bool queue_serial_output(SerialBuffer &buffer, const uint8_t *source, size_t size);
// Remove only the bytes the backend successfully transmitted.
void serial_bytes_sent(SerialBuffer &buffer, size_t bytes_sent);

#endif
