#pragma once
#include <stdint.h>

struct TestSpi { uint32_t CR1, CFG1, CFG2, SR, IFCR, TXDR; };
struct TestTimer { uint32_t PSC, ARR, DIER, CR1; };
extern TestSpi test_spi1, test_spi2;
extern TestTimer test_timer;
#define SPI1 (&test_spi1)
#define SPI2 (&test_spi2)
#define TIM7 (&test_timer)
enum { SPI_SR_UDR = 1, SPI_SR_TIFRE = 2, SPI_SR_TXP = 4,
       SPI_IFCR_UDRC = 1, SPI_IFCR_TIFREC = 2, SPI_CR1_SPE = 1, SPI_CR1_CSTART = 2,
       SPI_CFG1_TXDMAEN = 16, SPI_CFG1_RXDMAEN = 32,
       DMA_SxCR_CT = 1u << 19, DMA_SxCR_DBM = 1u << 18,
       SPI_CFG2_IOSWP = 1, TIM_DIER_UIE = 1, TIM_CR1_CEN = 1,
       TIM7_IRQn = 7, I2S_IT_TXP = 1, I2S_IT_UDR = 2, I2S_IT_FRE = 4 };
#define CLEAR_BIT(reg, bits) ((reg) &= ~(bits))
#define SET_BIT(reg, bits) ((reg) |= (bits))
#define __HAL_RCC_TIM7_CLK_ENABLE() ((void)0)
#define __HAL_I2S_CLEAR_UDRFLAG(handle) ((void)(handle))
#define __HAL_I2S_CLEAR_TIFREFLAG(handle) ((void)(handle))
#define __HAL_I2S_ENABLE(handle) ((handle)->Instance->CR1 |= SPI_CR1_SPE)
#define __HAL_I2S_DISABLE_IT(handle, bits) ((void)(handle), (void)(bits))
inline uint32_t test_interrupt_mask;
inline void (*test_sleep)();
inline void (*test_pending_interrupt)();
inline uint32_t __get_PRIMASK() { return test_interrupt_mask; }
inline void __disable_irq() { test_interrupt_mask = 1; }
inline void __set_PRIMASK(uint32_t mask) {
    test_interrupt_mask = mask;
    if (!mask && test_pending_interrupt) {
        auto handler = test_pending_interrupt;
        test_pending_interrupt = nullptr;
        handler();
    }
}
inline void __DSB() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline void __WFI() { if (test_sleep) test_sleep(); }
inline void __DMB() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline void HAL_NVIC_SetPriority(int, int, int) {}
inline void HAL_NVIC_EnableIRQ(int) {}
inline void HAL_Delay(unsigned) {}
[[noreturn]] void Error_Handler();
