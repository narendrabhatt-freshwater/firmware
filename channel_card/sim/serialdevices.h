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

#ifndef CHANNEL_SIM_SERIALDEVICES_H
#define CHANNEL_SIM_SERIALDEVICES_H

#include <stdint.h>

/* ---- serial devices ----------------------------------------------------- */

constexpr char SIM_USB_PATH[] = "/tmp/sim-usb";
constexpr char SIM_RS485_PATH[] = "/tmp/sim-rs485";

/* Paths must remain valid until close_serial_devices(); defaults are the
 * normal host endpoints. Tests use isolated paths without touching them. */
bool setup_serial_devices(const char *usb_path = SIM_USB_PATH,
                          const char *rs485_path = SIM_RS485_PATH);
bool process_serial_io(uint64_t deadlineNs);
void close_serial_devices(void);

/* ---- host timing -------------------------------------------------------- */

/* Call before opening serial ports. Returns false if scheduling setup fails. */
bool init_simulator_timing(void);
uint64_t get_simulator_time_ns(void);

#endif
