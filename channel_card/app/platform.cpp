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

#include "platform.h"

static const PlatformSynchronization *s_synchronization;

void set_platform_synchronization(const PlatformSynchronization *operations)
{
    s_synchronization = operations;
}

uint32_t disable_interrupts_and_save_state(void)
{
    return s_synchronization ? s_synchronization->save_interrupt_state() : 0u;
}

void restore_interrupt_state(uint32_t saved)
{
    if (s_synchronization) s_synchronization->restore_interrupt_state(saved);
}

void board_memory_barrier(void)
{
    if (s_synchronization) s_synchronization->memory_barrier();
    else __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
