/**
 ******************************************************************************
 * @file    uart5_rx.c
 * @brief   Interrupt-driven receive ring buffer for UART5 (RS485 console).
 *
 * Polling UART5 with HAL_UART_Receive() from the main loop only catches a
 * byte if the loop happens to look within one character time (~22 us at
 * 460800) — and the loop also runs the USB task, the cpuload producer and
 * blocking console replies. Hosts therefore had to pace every character
 * ~1 ms apart, which made a chord take ~100 ms to send.
 *
 * Buffering in the RXNE interrupt removes that constraint: characters are
 * captured regardless of what the main loop is doing, so the host can send
 * a whole command at wire speed.
 *
 * The hardware RX FIFO is re-enabled here (CubeMX leaves it off). That gives
 * ~16 character-times of slack if a higher-priority audio ISR runs long —
 * without it, priority-3 UART RX still overruns under I2S/USB load.
 *
 * Deliberately register-level rather than HAL_UART_Receive_IT(): the HAL
 * variant arms a fixed-length transfer and stops on completion, which does
 * not fit a console stream of arbitrary-length lines.
 ******************************************************************************
 */

#include "uart5_rx.h"

#include "main.h"
#include "usart.h"

/** Power of two so the wrap is a mask, not a modulo. 2048 B holds several
 * full 16-voice note bursts (~20 B each) so a mash of On+Off chords cannot
 * overrun while the main loop is in USB/audio work. 256 was too small: one
 * 16-note batch alone is ~300 B and dropped Offs left voices stuck. */
#define UART5_RX_BUF_SIZE 2048u
#define UART5_RX_BUF_MASK (UART5_RX_BUF_SIZE - 1u)

static volatile uint8_t rx_buf[UART5_RX_BUF_SIZE];
/** Written only by the ISR. */
static volatile uint16_t rx_head;
/** Written only by Uart5Rx_Get(). Single producer + single consumer, so no
 * critical section is needed around either index. */
static volatile uint16_t rx_tail;
static volatile uint32_t rx_dropped;

void Uart5Rx_Init(void)
{
  rx_head = 0u;
  rx_tail = 0u;
  rx_dropped = 0u;

  /* CubeMX calls DisableFifoMode(); turn it back on so a late ISR still
   * finds the burst sitting in hardware instead of setting ORE. */
  if (HAL_UARTEx_SetRxFifoThreshold(&huart5, UART_RXFIFO_THRESHOLD_1_8) !=
      HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_EnableFifoMode(&huart5) != HAL_OK)
  {
    Error_Handler();
  }

  /* Discard boot/turnaround noise and clear latched errors before arming:
   * a sticky ORE would stop RXFNE from ever firing again. */
  while (UART5->ISR & USART_ISR_RXNE_RXFNE)
  {
    (void)UART5->RDR;
  }
  UART5->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;

  /* Preempt 0 — must be ≥ audio DMA (also 0). Was 1: under a held 16-voice
   * chord NoteBank DMA (prio 0) nested over UART and dropped host nX bytes
   * → no exec → no [C]ok → upstream MIDI session fail-stop on a late slot (e.g. nE).
   * Handler only drains RDR into the ring; keep it short. */
  HAL_NVIC_SetPriority(UART5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(UART5_IRQn);

  UART5->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
}

uint8_t Uart5Rx_Get(uint8_t *out)
{
  if (out == NULL || rx_tail == rx_head)
  {
    return 0u;
  }
  *out = rx_buf[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1u) & UART5_RX_BUF_MASK);
  return 1u;
}

uint32_t Uart5Rx_DroppedCount(void)
{
  return rx_dropped;
}

void UART5_IRQHandler(void)
{
  uint32_t isr = UART5->ISR;

  if (isr & USART_ISR_ORE)
  {
    /* Clear or RXFNE stalls. The surviving byte is still in RDR/FIFO and
     * will be pulled by the drain loop below. */
    UART5->ICR = USART_ICR_ORECF;
    rx_dropped++;
  }

  while (isr & USART_ISR_RXNE_RXFNE)
  {
    const uint8_t c = (uint8_t)(UART5->RDR & 0xFFu);

    /* RS485 turnaround often stamps FE/NE on noise and on real bytes.
     * Clear the sticky flags so RX keeps running, but keep the byte —
     * dropping on FE used to truncate frames and stick voices.
     * Console_Poll resyncs on a fresh "c:"/"*:"/"e:" if idle garbage
     * prefixes a line. */
    if (isr & (USART_ISR_FE | USART_ISR_NE))
    {
      UART5->ICR = USART_ICR_FECF | USART_ICR_NECF;
    }

    const uint16_t next = (uint16_t)((rx_head + 1u) & UART5_RX_BUF_MASK);
    if (next == rx_tail)
    {
      /* Full: drop the new byte rather than overwrite an unread command. */
      rx_dropped++;
    }
    else
    {
      rx_buf[rx_head] = c;
      rx_head = next;
    }
    isr = UART5->ISR;
  }
}
