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

#include "backend_model.h"
#include "serial_buffer.h"
#include "platform.h"
#include "application.h"
#include "usb.h"
#include <algorithm>
#include <cstring>

namespace model {
uint32_t time_ms, read_limit = 64;
std::vector<uint8_t> usb_rx, usb_tx, rs_rx, rs_tx;

void reconnect()
{
    ++usb_serial_buffer.generation;
    usb_serial_buffer.connected = true;
    usb_serial_buffer.faulted = false;
    usb_serial_buffer.input_size = usb_serial_buffer.output_size = 0;
    usb_rx.clear(); usb_tx.clear();
}

void service_usb()
{
    SerialBuffer &buffer = usb_serial_buffer;
    if (buffer.connected && !buffer.faulted) {
        size_t n = std::min(usb_rx.size(), std::min(size_t(read_limit), sizeof buffer.input - buffer.input_size));
        if (n) std::memcpy(buffer.input + buffer.input_size, usb_rx.data(), n);
        buffer.input_size += n;
        usb_rx.erase(usb_rx.begin(), usb_rx.begin() + n);
    }
    process_usb_messages();
    usb_tx.insert(usb_tx.end(), buffer.output, buffer.output + buffer.output_size);
    buffer.output_size = 0;
}

void service_rs485()
{
    SerialBuffer &buffer = rs485_serial_buffer;
    size_t n = std::min(rs_rx.size(), sizeof buffer.input - buffer.input_size);
    if (n) std::memcpy(buffer.input + buffer.input_size, rs_rx.data(), n);
    buffer.input_size += n;
    rs_rx.erase(rs_rx.begin(), rs_rx.begin() + n);
    process_rs485_commands();
    if (buffer.clear_input) {
        rs_rx.clear(); buffer.input_size = 0; buffer.clear_input = false;
    }
    rs_tx.insert(rs_tx.end(), buffer.output, buffer.output + buffer.output_size);
    buffer.output_size = 0;
}
}

uint32_t get_board_time_ms() { return model::time_ms++; }
void init_usb_device() { model::reconnect(); }

/* ---- leave RS485 output queued until the model services the transmitter --- */

void write_serial(const std::vector<uint8_t> &)
{
}

/* ---- leave USB output queued until the model services the transmitter ----- */

void write_usb(const std::vector<uint8_t> &)
{
}
