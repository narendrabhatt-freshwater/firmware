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

#ifndef CHANNEL_APPLICATION_H
#define CHANNEL_APPLICATION_H

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

/* Channel Card RS485 + USB CDC console.
 * One command parser serves both transports. RS485 uses card address prefixes
 * (`c:` / `*:` / bare) and tagged replies (`[C]ok` / `[C]err:<token>`).
 * Stable error tokens: syntax, range, unknown, rxdrop.
 * Bring-up: initialize platform devices and RS485 reception before init_application().
 * Poll from the main loop after servicing the USB task. */

/**
 * @brief Drain available RS485 bytes and execute addressed commands.
 * @note Replies may wait for bus access/transmission within existing deadlines.
 */
void init_application(void);
void process_application(void);
void consume_serial_output(int count);
void consume_usb_output(int count);
void reset_usb_connection(void);
void process_rs485_commands(void);

/**
 * @brief Run one console command line that arrived over USB CDC.
 * @param line NUL-terminated command (mutated by the parser). May be NULL
 *             (no-op). Replies are routed to CDC for the duration of the call.
 */
void execute_usb_command(char *line);

/* ---- upload compiled voice programs ------------------------------------- */

uint8_t is_script_upload_active(void);
int begin_script_upload(uint8_t voice, uint32_t nbytes);
uint32_t append_script_upload_bytes(const uint8_t *data, uint32_t size);
void abort_script_upload(void);

#ifdef __cplusplus
}
#endif

#endif
