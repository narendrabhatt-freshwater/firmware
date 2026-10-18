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

#ifndef CHANNEL_APP_CHANNEL_H
#define CHANNEL_APP_CHANNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional physical outputs, bound by firmware before application startup.
 * Missing handlers accept logical requests without driving an output, keeping
 * headless protocol/script behavior. Tables must have static lifetime. */
typedef struct {
    int (*set_led_colour)(float red, float green, float blue, float brightness);
    int (*set_gain)(char channel, uint8_t half_db);
} ChannelOutputs;
void set_channel_outputs(const ChannelOutputs *outputs);

/* ---- request a logical colour ------------------------------------------- */

int set_channel_led_colour(float red, float green, float blue, float brightness);

/* ---- initialize and poll the console ------------------------------------ */

/* Channel Card RS485 console, with card address prefixes
 * (`c:` / `*:` / bare) and tagged replies (`[C]ok` / `[C]err:<token>`).
 * Stable error tokens: syntax, range, unknown, rxdrop.
 * Bring-up: initialize platform devices and RS485 reception before init_channel_card().
 * Poll from the main loop after servicing the USB task. */

/**
 * @brief Initialize application state and emit the ready banner.
 * @note Also initializes note filter / bank / envelope cold state.
 */
void init_channel_card(void);

/**
 * @brief Drain available RS485 bytes and execute addressed commands.
 * @note Replies may wait for bus access/transmission within existing deadlines.
 */
void process_rs485_commands(void);

#ifdef __cplusplus
}
#endif

#endif
