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
#include "board.h"
#include "usb.h"

/* ---- keep application services running during an rs485 reply ------------ */

int write_rs485_bytes(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
    uint32_t started = get_board_time_ms();
    if (start_rs485_transmit(data, size) != 0) return -1;
    int state = poll_rs485_transmit();
    while (state == 0) {
        process_usb_messages();
        state = poll_rs485_transmit();
        if (state == 0 && (uint32_t)(get_board_time_ms() - started) >= timeout_ms) {
            abort_rs485_transmit();
            return -1;
        }
    }
    if (state < 0) abort_rs485_transmit();
    return state > 0 ? 0 : -1;
}
