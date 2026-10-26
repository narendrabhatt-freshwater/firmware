#include "board.h"
#include "channel.h"

using namespace std;

/* ---- return the shared transceiver and TX pin to their idle state -------- */

static void release_bus()
{
    write_pin(GPIOB, 3, false);
    for (volatile unsigned i(0); i < 300; ++i) {}
    configure_pin(GPIOB, 3, 0);
    configure_pin(GPIOC, 12, 0, 0, 1);
}

/* ---- acquire the shared transceiver for transmission -------------------- */

static void acquire_bus()
{
    configure_pin(GPIOC, 12, 2, 8, 0, 3);
    write_pin(GPIOB, 3, true);
    configure_pin(GPIOB, 3, 1, 0, 0, 3);
    for (volatile unsigned i(0); i < 300; ++i) {}
}

/* ---- configure UART reception and hardware error interrupts ------------- */

void init_serial()
{
    RCC->APB1LENR |= RCC_APB1LENR_UART5EN;
    (void)RCC->APB1LENR;
    release_bus();
    configure_pin(GPIOD, 2, 2, 8, 0, 3);
    UART5->CR1 = 0;
    UART5->CR2 = 0;
    UART5->CR3 = USART_CR3_EIE;
    UART5->PRESC = 0;
    UART5->BRR = (peripheral_clock_hz + RS485_BAUD / 2) / RS485_BAUD;
    UART5->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE | USART_CR1_FIFOEN |
                 USART_CR1_PEIE;
    NVIC_SetPriority(UART5_IRQn, 0);
    NVIC_EnableIRQ(UART5_IRQn);
}

/* ---- send bytes directly and release the bus after the final stop bit --- */

void write_serial(vector<uint8_t> const& bytes)
{
    if (bytes.empty()) return;
    while ((UART5->ISR & USART_ISR_BUSY) ||
           (GPIOB->IDR & (1u << 3))) {}
    UART5->CR1 &= ~USART_CR1_RE;
    acquire_bus();
    UART5->ICR = USART_ICR_TCCF;
    for (uint8_t byte : bytes) {
        while (!(UART5->ISR & USART_ISR_TXE_TXFNF)) {}
        UART5->TDR = byte;
    }
    while (!(UART5->ISR & USART_ISR_TC)) {}
    UART5->ICR = USART_ICR_TCCF;
    release_bus();
    UART5->CR1 |= USART_CR1_RE;
}

/* ---- read available UART bytes directly into the application input ------ */

bool process_serial(vector<uint8_t>& bytes)
{
    while (bytes.size() < 64 && (UART5->ISR & USART_ISR_RXNE_RXFNE))
        bytes.push_back(uint8_t(UART5->RDR));
    return !bytes.empty();
}

/* ---- halt on a UART hardware error ------------------------------------- */

extern "C" void UART5_IRQHandler()
{ fail(); }
