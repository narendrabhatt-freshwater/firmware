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
#include "channel.h"
#include "serial_buffer.h"
#include <string.h>

/* ---- progress rs485 replies without waiting ---------------------------- */

struct Rs485Reply {
    uint8_t data[RS485_REPLY_BYTES];
    uint16_t size;
    Rs485ReplyCompletion completed;
};
static Rs485Reply replies[RS485_REPLY_SLOTS];
static unsigned reply_head;
static unsigned reply_count;
static bool reply_queued;

/* ---- retain a complete reply until its bytes have been sent -------------- */

int queue_rs485_reply(const uint8_t *data, uint16_t size, Rs485ReplyCompletion completed)
{
    if (!data || !size || size > RS485_REPLY_BYTES || reply_count == RS485_REPLY_SLOTS) return -1;
    Rs485Reply &reply = replies[(reply_head + reply_count) % RS485_REPLY_SLOTS];
    memcpy(reply.data, data, size);
    reply.size = size;
    reply.completed = completed;
    ++reply_count;
    return 0;
}

/* ---- check whether a reply still needs to be transmitted ----------------- */

uint8_t has_pending_rs485_reply(void)
{
    return reply_count != 0u;
}

/* ---- complete a transmitted reply --------------------------------------- */

static void finish_rs485_reply(uint8_t success)
{
    Rs485ReplyCompletion completed = replies[reply_head].completed;
    reply_head = (reply_head + 1u) % RS485_REPLY_SLOTS;
    --reply_count;
    reply_queued = false;
    if (completed) completed(success);
}

/* ---- transmit queued replies until finished or the port is full ---------- */

void process_rs485_replies(void)
{
    while (reply_count)
    {
        if (!reply_queued)
        {
            const Rs485Reply &reply = replies[reply_head];
            rs485_serial_buffer.transmit_failed = false;
            if (!queue_serial_output(rs485_serial_buffer, reply.data, reply.size))
            {
                return;
            }
            reply_queued = true;
        }
        while (rs485_serial_buffer.output_size)
        {
            size_t before = rs485_serial_buffer.output_size;
            write_serial({rs485_serial_buffer.output,
                rs485_serial_buffer.output + before});
            if (rs485_serial_buffer.output_size == before)
            {
                return;
            }
        }
        finish_rs485_reply(!rs485_serial_buffer.transmit_failed);
    }
}
