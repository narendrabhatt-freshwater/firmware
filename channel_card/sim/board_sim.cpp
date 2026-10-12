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

#include "board.h"
#include "transport.h"
#include "audio.h"
#include "serialdevices.h"
#include <time.h>

/* ---- read host time ----------------------------------------------------- */

uint64_t get_simulator_time_ns(void)
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}

uint32_t get_board_time_ms(void)
{
    static const uint64_t start = get_simulator_time_ns();
    return (uint32_t)((get_simulator_time_ns() - start) / 1000000);
}

/* ---- unused physical outputs ------------------------------------------- */

/* Satisfy the shared application interface without modeling board hardware. */
void write_board_led(uint8_t, uint8_t) {}
void init_board_rgb_led(void) {}
int set_board_rgb_colour(float, float, float, float) { return 0; }
void service_board_leds(void) {}
void write_board_switch(uint8_t, uint8_t) {}
uint8_t is_dac_ready(void) { return 1; }
void set_dac_channel_trim(char, uint8_t) {}

/* PTYs have no shared electrical bus; serialdevices.cpp owns byte transport. */
void release_rs485_bus(void) {}
void acquire_rs485_bus(void) {}
uint8_t is_rs485_bus_free(void) { return 1; }
uint32_t get_rs485_baud_rate(void) { return FW_RS485_BAUD; }

/* ---- single-threaded execution ------------------------------------------ */

/* The simulator has one thread. Its application and device callbacks never
 * overlap; these are not suitable implementations for a threaded backend. */
uint32_t disable_interrupts_and_save_state(void) { return 0; }
void restore_interrupt_state(uint32_t) {}
void board_memory_barrier(void) {}
/* No DMA refills can overlap; retain the existing diagnostic reply fields. */
uint32_t get_audio_refill_overlap_count(void) { return 0; }
void clear_audio_output_statistics(void) {}
