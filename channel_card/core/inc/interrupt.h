#ifndef CHANNEL_INTERRUPT_H
#define CHANNEL_INTERRUPT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Counting wake-up semaphore shared by USB, UART, DMA and deadline ticks. */
void signal_interrupt(void);
void wait_interrupt(void);
#ifdef __cplusplus
}
#endif
#endif
