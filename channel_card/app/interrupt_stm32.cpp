#include "main.h"
#include <interrupt.h>

static uint32_t pending;

void signal_interrupt(void)
{
    __atomic_fetch_add(&pending, 1u, __ATOMIC_RELEASE);
}

void wait_interrupt(void)
{
    for (;;) {
        const uint32_t saved = __get_PRIMASK();
        __disable_irq();
        if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_sub(&pending, 1u, __ATOMIC_RELAXED);
            __set_PRIMASK(saved);
            return;
        }
        // Keep the test and sleep atomic. A newly pending interrupt wakes WFI
        // even with PRIMASK set; restoring PRIMASK then runs its handler.
        __DSB();
        __WFI();
        __set_PRIMASK(saved);
    }
}
