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

#ifndef CHANNEL_APP_PLATFORM_H
#define CHANNEL_APP_PLATFORM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds since start, wrapping at 2^32. Every platform supplies a clock. */
uint32_t get_board_time_ms(void);

/* Single-threaded execution is the default. A concurrent platform binds all
 * three synchronization operations before application work or interrupts
 * begin. The table must remain valid for the lifetime of the application. */
typedef struct {
    uint32_t (*save_interrupt_state)(void);
    void (*restore_interrupt_state)(uint32_t saved);
    void (*memory_barrier)(void);
} PlatformSynchronization;
void set_platform_synchronization(const PlatformSynchronization *operations);
uint32_t disable_interrupts_and_save_state(void);
void restore_interrupt_state(uint32_t saved);
void board_memory_barrier(void);

#if defined(CHANNEL_ENABLE_CYCLE_PROFILING)
void init_board_cycle_counter(void);
uint32_t get_board_cycle_count(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
