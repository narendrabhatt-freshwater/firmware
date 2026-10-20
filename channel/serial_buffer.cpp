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

#include "serial_buffer.h"
#include <cstring>

SerialBuffer usb_serial_buffer;
SerialBuffer rs485_serial_buffer;

/* ---- record accepted serial input -------------------------------------- */

void serial_bytes_received(SerialBuffer &buffer, size_t bytes_received)
{
    buffer.input_size += bytes_received;
}

/* ---- consume a prefix of serial input ---------------------------------- */

size_t read_serial_input(SerialBuffer &buffer, uint8_t *destination, size_t size)
{
    if (size > buffer.input_size)
    {
        size = buffer.input_size;
    }
    if (!size)
    {
        return 0;
    }
    std::memcpy(destination, buffer.input, size);
    buffer.input_size -= size;
    std::memmove(buffer.input, buffer.input + size, buffer.input_size);
    return size;
}

/* ---- queue a complete serial output message ---------------------------- */

bool queue_serial_output(SerialBuffer &buffer, const uint8_t *source, size_t size)
{
    if (size > sizeof buffer.output - buffer.output_size)
    {
        return false;
    }
    if (size)
    {
        std::memcpy(buffer.output + buffer.output_size, source, size);
    }
    buffer.output_size += size;
    return true;
}

/* ---- remove only the transmitted output prefix ------------------------- */

void serial_bytes_sent(SerialBuffer &buffer, size_t bytes_sent)
{
    buffer.output_size -= bytes_sent;
    std::memmove(buffer.output, buffer.output + bytes_sent, buffer.output_size);
}
