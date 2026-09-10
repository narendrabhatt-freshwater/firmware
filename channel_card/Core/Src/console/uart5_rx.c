/**
 ******************************************************************************
 * @file    uart5_rx.c
 * @brief   Interrupt-driven receive ring buffer for UART5 (RS485 console).
 *
 * Polling UART5 with HAL_UART_Receive() from the main loop only catches a
 * byte if the loop happens to look within one character time — and the loop also runs the USB task, the cpuload producer and
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
static volatile uint8_t tx_complete;

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

  /* Release RS485 at the final stop bit, even while the audio pump runs. */
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

void Uart5Rx_Clear(void)
{
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  UART5->RQR = USART_RQR_RXFRQ;
  UART5->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF |
               USART_ICR_PECF;
  rx_tail = rx_head;
  __set_PRIMASK(primask);
}

uint32_t Uart5Rx_DroppedCount(void)
{
  return rx_dropped;
}

int Uart5Rx_Transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
  uint32_t const started = HAL_GetTick();
  if (data == NULL || size == 0u) return -1;
  tx_complete = 0u;
  UART5->ICR = USART_ICR_TCCF;
  for (uint16_t i = 0u; i < size; ++i)
  {
    while ((UART5->ISR & USART_ISR_TXE_TXFNF) == 0u)
      if (HAL_GetTick() - started >= timeout_ms) return -1;
    if (i + 1u == size)
    {
      uint32_t const primask = __get_PRIMASK();
      __disable_irq();
      UART5->TDR = data[i];
      UART5->CR1 |= USART_CR1_TCIE;
      __set_PRIMASK(primask);
    }
    else UART5->TDR = data[i];
  }
  while (tx_complete == 0u)
  {
    if (HAL_GetTick() - started >= timeout_ms)
    {
      UART5->CR1 &= ~USART_CR1_TCIE;
      return -1;
    }
  }
  return 0;
}

void UART5_IRQHandler(void)
{
  uint32_t isr = UART5->ISR;

  if ((isr & USART_ISR_TC) != 0u && (UART5->CR1 & USART_CR1_TCIE) != 0u)
  {
    RS485_CTL_GPIO_Port->BSRR = (uint32_t)RS485_CTL_Pin << 16u;
    UART5->CR1 &= ~USART_CR1_TCIE;
    UART5->ICR = USART_ICR_TCCF;
    tx_complete = 1u;
  }

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

    /* RS485 turnaround can stamp FE/NE on noise and valid bytes.
     * Clear the sticky flags so RX keeps running, and retain the byte to avoid
     * truncating command frames.
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
