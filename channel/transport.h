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

#ifndef CHANNEL_TRANSPORT_H
#define CHANNEL_TRANSPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Nonblocking reply queue. Copy the complete frame or reject it; completion
 * is reported after the platform accepts the final output bytes.
 * Capacity is one VM-memory response: one summary and eight voice records.
 * Command parsing must stop while any response is pending. */
#define RS485_REPLY_BYTES 224u
#define RS485_REPLY_SLOTS 9u
typedef void (*Rs485ReplyCompletion)(uint8_t success);
int queue_rs485_reply(const uint8_t *data, uint16_t size, Rs485ReplyCompletion completed);
void process_rs485_replies(void);
uint8_t has_pending_rs485_reply(void);

#ifdef __cplusplus
}
#endif
#endif
