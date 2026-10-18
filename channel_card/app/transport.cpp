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

#include "transport.h"
#include "platform.h"
#include "usb.h"

/* ---- keep application services running during an rs485 reply ------------ */

static const Rs485BusServices *s_bus;
static TransportTimeouts s_timeouts{};

void set_transport_timeouts(const TransportTimeouts *timeouts)
{
    s_timeouts = timeouts ? *timeouts : TransportTimeouts{};
}

const TransportTimeouts *get_transport_timeouts(void) { return &s_timeouts; }

void set_rs485_bus_services(const Rs485BusServices *services)
{
    s_bus = services;
}

uint32_t get_rs485_baud_rate(void)
{
    return s_bus ? s_bus->baud_rate() : FW_RS485_BAUD;
}

uint8_t wait_for_rs485_bus(uint32_t timeout_ms)
{
    if (!s_bus) return 1u;
    uint32_t started = get_board_time_ms();
    while (!s_bus->is_free()) {
        if (timeout_ms && (uint32_t)(get_board_time_ms() - started) >= timeout_ms) return 0u;
    }
    return 1u;
}

int write_rs485_bytes(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
    if (s_bus) s_bus->acquire();
    uint32_t started = get_board_time_ms();
    if (start_rs485_transmit(data, size) != 0) {
        if (s_bus) s_bus->release();
        return -1;
    }
    int state = poll_rs485_transmit();
    while (state == 0) {
        process_usb_messages();
        state = poll_rs485_transmit();
        if (state == 0 && timeout_ms && (uint32_t)(get_board_time_ms() - started) >= timeout_ms) {
            abort_rs485_transmit();
            if (s_bus) s_bus->release();
            return -1;
        }
    }
    if (state < 0) abort_rs485_transmit();
    if (s_bus) s_bus->release();
    return state > 0 ? 0 : -1;
}
