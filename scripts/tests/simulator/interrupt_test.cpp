#include "../../../channel_card/app/interrupt_stm32.cpp"
#include <cassert>

static unsigned sleeps;
static void interrupt_during_sleep()
{
    assert(test_interrupt_mask == 1);
    ++sleeps;
    // A peripheral IRQ may wake the CPU without posting our semaphore.
    // The next IRQ posts it, including when it arrived with IRQs masked.
    test_pending_interrupt = sleeps == 1 ? +[] {} : signal_interrupt;
}

int main()
{
    test_sleep = interrupt_during_sleep;
    signal_interrupt();
    signal_interrupt();
    wait_interrupt();
    assert(sleeps == 0 && test_interrupt_mask == 0);
    wait_interrupt();
    assert(sleeps == 0 && test_interrupt_mask == 0);
    wait_interrupt();
    assert(sleeps == 2 && test_interrupt_mask == 0);
    // Waiting for an already posted signal preserves the caller's IRQ mask.
    __disable_irq();
    signal_interrupt();
    wait_interrupt();
    assert(sleeps == 2 && test_interrupt_mask == 1);
}
