#include "board.h"
#include "channel.h"
#include "cs4304.h"
#include "usb.h"
#include "main.h"
#include "usart.h"
#include <stddef.h>

/* ---- read time and mask interrupts -------------------------------------- */

/* STM32H725 implementation of board.h, plus the UART5 driver declared in
 * channel.h. The shared console in channel.cpp reaches hardware only here. */

uint32_t Board_Millis(void)
{
    return HAL_GetTick();
}

uint32_t Board_IrqSave(void)
{
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    return saved;
}

void Board_IrqRestore(uint32_t saved)
{
    __set_PRIMASK(saved);
}

/* ---- drive leds and analog switches ------------------------------------- */

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} BoardPin;

/* Indexed by the BOARD_LED_* and BOARD_SWITCH_* values in board.h. */
static const BoardPin led_pins[BOARD_LED_COUNT] = {
    {LED_R_GPIO_Port, LED_R_Pin},
    {LED_Y_GPIO_Port, LED_Y_Pin},
    {RGB_R_GPIO_Port, RGB_R_Pin},
    {RGB_G_GPIO_Port, RGB_G_Pin},
    {RGB_B_GPIO_Port, RGB_B_Pin},
};

static const BoardPin switch_pins[BOARD_SWITCH_COUNT] = {
    {HP_SW_GPIO_Port, HP_SW_Pin},
    {BP_SW_GPIO_Port, BP_SW_Pin},
    {LP_SW_GPIO_Port, LP_SW_Pin},
    {VCA_SW_GPIO_Port, VCA_SW_Pin},
    {BYPASS_SW_GPIO_Port, BYPASS_SW_Pin},
    {VCF_SW_GPIO_Port, VCF_SW_Pin},
    {SCF_SW_GPIO_Port, SCF_SW_Pin},
    {HP_CTL_GPIO_Port, HP_CTL_Pin},
};

void Board_LedWrite(uint8_t led, uint8_t on)
{
    if (led < BOARD_LED_COUNT)
        HAL_GPIO_WritePin(led_pins[led].port, led_pins[led].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Board_SwitchWrite(uint8_t sw, uint8_t high)
{
    if (sw < BOARD_SWITCH_COUNT)
        HAL_GPIO_WritePin(switch_pins[sw].port, switch_pins[sw].pin,
                          high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- set dac attenuation ------------------------------------------------ */

/* Bound from main via ChannelConsole_SetDacHandle(). */
static CS4304_HandleTypeDef *s_dac;

void ChannelConsole_SetDacHandle(CS4304_HandleTypeDef *h)
{
    s_dac = h;
}

uint8_t Board_DacReady(void)
{
    return s_dac != NULL;
}

void Board_DacSetTrim(char channel, uint8_t trim)
{
    if (s_dac != NULL) CS4304_SetChannelTrim(s_dac, channel, trim);
}

/* ---- receive and transmit console bytes --------------------------------- */

/* Interrupt-driven receive ring buffer for UART5 (RS485 console).
 * Polling UART5 with HAL_UART_Receive() from the main loop only catches a
 * byte if the loop happens to look within one character time — and the loop also runs the USB task, the cpuload producer and
 * blocking console replies. Hosts therefore had to pace every character
 * ~1 ms apart, which made a chord take ~100 ms to send.
 * Buffering in the RXNE interrupt removes that constraint: characters are
 * captured regardless of what the main loop is doing, so the host can send
 * a whole command at wire speed.
 * The hardware RX FIFO is re-enabled here (CubeMX leaves it off). That gives
 * ~16 character-times of slack if a higher-priority audio ISR runs long —
 * without it, priority-3 UART RX still overruns under I2S/USB load.
 * Deliberately register-level rather than HAL_UART_Receive_IT(): the HAL
 * variant arms a fixed-length transfer and stops on completion, which does
 * not fit a console stream of arbitrary-length lines. */

#define UART5_RX_BUF_SIZE 8192u
#define UART5_RX_BUF_MASK (UART5_RX_BUF_SIZE - 1u)
static_assert(UART5_RX_BUF_SIZE > 128u * 32u, "RX must hold the worst-case note burst");
static_assert((UART5_RX_BUF_SIZE & UART5_RX_BUF_MASK) == 0u, "RX size must be a power of two");

static volatile uint8_t rx_buf[UART5_RX_BUF_SIZE];
/** Written only by the ISR. */
static volatile uint16_t rx_head;
/** Written only by Uart5Rx_Get(). Single producer + single consumer, so no
 * critical section is needed around either index. */
static volatile uint16_t rx_tail;
static volatile uint32_t rx_dropped;
static volatile uint8_t tx_complete;
static const uint8_t *volatile tx_next;
static volatile uint16_t tx_left;

/* ---- initialize rs485 reception ----------------------------------------- */

void Uart5Rx_Init(void)
{
    rx_head = 0u;
    rx_tail = 0u;
    rx_dropped = 0u;

    /* CubeMX calls DisableFifoMode(); turn it back on so a late ISR still
     * finds the burst sitting in hardware instead of setting ORE. */
    if (HAL_UARTEx_SetRxFifoThreshold(&huart5, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_UARTEx_EnableFifoMode(&huart5) != HAL_OK) {
        Error_Handler();
    }

    /* Discard boot/turnaround noise and clear latched errors before arming:
     * a sticky ORE would stop RXFNE from ever firing again. */
    while (UART5->ISR & USART_ISR_RXNE_RXFNE) {
        (void)UART5->RDR;
    }
    UART5->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;

    /* Release RS485 at the final stop bit, even while the audio pump runs. */
    HAL_NVIC_SetPriority(UART5_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(UART5_IRQn);

    UART5->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
}

/* ---- read and clear received bytes -------------------------------------- */

uint8_t Uart5Rx_Get(uint8_t *out)
{
    if (out == NULL || rx_tail == rx_head) {
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
    UART5->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    rx_tail = rx_head;
    __set_PRIMASK(primask);
}

uint32_t Uart5Rx_DroppedCount(void)
{
    return rx_dropped;
}

/* ---- transmit an rs485 reply -------------------------------------------- */

int Uart5Rx_Transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
    uint32_t const started = HAL_GetTick();
    if (data == NULL || size == 0u) return -1;
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    UART5->CR1 &= ~(USART_CR1_TCIE | USART_CR1_TXEIE_TXFNFIE);
    tx_complete = 0u;
    tx_next = data;
    tx_left = size;
    UART5->ICR = USART_ICR_TCCF;
    UART5->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    __set_PRIMASK(primask);
    /* RX/TC already run above the mixer. Feed TX there too, so a mixer callback
     * cannot leave gaps in a 61-byte status reply. The stack buffer remains live
     * until TC. USB dispatch cannot recurse into this RS485-only transmitter. */
    while (tx_complete == 0u) {
        USB_App_Task();
        if (!tx_complete && HAL_GetTick() - started >= timeout_ms) {
            uint32_t saved = __get_PRIMASK();
            __disable_irq();
            UART5->CR1 &= ~(USART_CR1_TCIE | USART_CR1_TXEIE_TXFNFIE);
            tx_left = 0u;
            __set_PRIMASK(saved);
            return -1;
        }
    }
    return 0;
}

/* ---- service the uart interrupt ----------------------------------------- */

extern "C" void UART5_IRQHandler(void)
{
    uint32_t isr = UART5->ISR;

    if ((isr & USART_ISR_TC) != 0u && (UART5->CR1 & USART_CR1_TCIE) != 0u) {
        RS485_CTL_GPIO_Port->BSRR = (uint32_t)RS485_CTL_Pin << 16u;
        UART5->CR1 &= ~USART_CR1_TCIE;
        UART5->ICR = USART_ICR_TCCF;
        tx_complete = 1u;
    }

    if ((UART5->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0u) {
        while (tx_left && (UART5->ISR & USART_ISR_TXE_TXFNF)) {
            UART5->TDR = *tx_next++;
            --tx_left;
        }
        if (!tx_left) {
            UART5->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
            UART5->CR1 |= USART_CR1_TCIE;
        }
    }

    if (isr & USART_ISR_ORE) {
        /* Clear or RXFNE stalls. The surviving byte is still in RDR/FIFO and
         * will be pulled by the drain loop below. */
        UART5->ICR = USART_ICR_ORECF;
        rx_dropped++;
    }

    while (isr & USART_ISR_RXNE_RXFNE) {
        const uint8_t c = (uint8_t)(UART5->RDR & 0xFFu);

        /* RS485 turnaround can stamp FE/NE on noise and valid bytes.
         * Clear the sticky flags so RX keeps running, and retain the byte to avoid
         * truncating command frames.
         * Console_Poll resyncs on a fresh "c:"/"*:"/"e:" if idle garbage
         * prefixes a line. */
        if (isr & (USART_ISR_FE | USART_ISR_NE)) {
            UART5->ICR = USART_ICR_FECF | USART_ICR_NECF;
        }

        const uint16_t next = (uint16_t)((rx_head + 1u) & UART5_RX_BUF_MASK);
        if (next == rx_tail) {
            /* Full: drop the new byte rather than overwrite an unread command. */
            rx_dropped++;
        } else {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
        isr = UART5->ISR;
    }
}

/* ---- release and acquire the shared rs485 bus --------------------------- */

/** Tri-state CTL and TX so the other MCU can use the bus (idle state). */
void Board_Rs485Release(void)
{
    GPIO_InitTypeDef gi = {};

    /* Actively drive DE low first (the other card's CTL is Hi-Z input, so
     * no contention) — a fast clean falling edge instead of the slow 10 k
     * pull-down decay.  TX is still driving mark while DE falls, so the
     * transceiver can't clock out garbage during the turnaround. */
    HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_RESET);
    for (volatile uint32_t i = 0; i < 300; i++) {
    }

    gi.Pin = RS485_CTL_Pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_NOPULL; /* external 10 k pull-down keeps DE low */
    HAL_GPIO_Init(RS485_CTL_GPIO_Port, &gi);

    gi.Pin = GPIO_PIN_12; /* PC12 = UART5_TX, shared D net */
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP; /* keep D at mark (idle) level */
    HAL_GPIO_Init(GPIOC, &gi);
}

/** Take the bus: TX back to UART AF, CTL driven HIGH (transmit mode). */
void Board_Rs485Acquire(void)
{
    GPIO_InitTypeDef gi = {};

    gi.Pin = GPIO_PIN_12; /* PC12 = UART5_TX */
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF8_UART5;
    HAL_GPIO_Init(GPIOC, &gi);

    HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_SET);
    gi.Pin = RS485_CTL_Pin;
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = 0;
    HAL_GPIO_Init(RS485_CTL_GPIO_Port, &gi);

    /* DE enable settle time (~1 us) before the first start bit */
    for (volatile uint32_t i = 0; i < 300; i++) {
    }
}

/** Check whether the RS485 bus is free (CTL low = no one transmitting).
 * Our own CTL is Hi-Z input while idle, so this reads the real DE line. */
uint8_t Board_Rs485BusFree(void)
{
    return (HAL_GPIO_ReadPin(RS485_CTL_GPIO_Port, RS485_CTL_Pin) == GPIO_PIN_RESET);
}

uint32_t Board_Rs485Baud(void)
{
    return huart5.Init.BaudRate;
}
