#include "board.h"
#include <cerrno>
#include <cstddef>
#include <initializer_list>
#include <sys/stat.h>

static volatile uint32_t milliseconds;

/* ---- select the board's direct SMPS supply before RAM initialization ----- */

extern "C" void ExitRun0Mode()
{
    PWR->CR3 &= ~PWR_CR3_LDOEN;
    while (!(PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) {}
}

/* ---- enable the FPU and establish the reset clock and memory map --------- */

extern "C" void SystemInit()
{
    SCB->CPACR |= 0xfu << 20;
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
    RCC->CFGR = 0;
    while (RCC->CFGR & RCC_CFGR_SWS) {}
    RCC->CR &= ~(RCC_CR_PLL1ON | RCC_CR_PLL2ON | RCC_CR_PLL3ON |
                 RCC_CR_HSEON | RCC_CR_HSI48ON);
    while (RCC->CR & (RCC_CR_PLL1RDY | RCC_CR_HSERDY)) {}
    RCC->CIER = 0;
    SCB->VTOR = FLASH_BASE;
    MPU->CTRL = 0;
    MPU->RNR = 0;
    MPU->RBAR = 0;
    MPU->RASR = MPU_RASR_XN_Msk | MPU_RASR_S_Msk |
                (0x87u << MPU_RASR_SRD_Pos) | (31u << MPU_RASR_SIZE_Pos) |
                MPU_RASR_ENABLE_Msk;
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();
}

/* ---- initialize MCU clocks and the board pins used by this firmware ----- */

void init_board()
{
    PWR->D3CR &= ~PWR_D3CR_VOS;
    (void)PWR->D3CR;
    while (!(PWR->D3CR & PWR_D3CR_VOSRDY)) {}
    RCC->CR |= RCC_CR_HSEBYP | RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {}
    RCC->PLLCKSELR = RCC_PLLCKSELR_PLLSRC_HSE | (3u << RCC_PLLCKSELR_DIVM1_Pos);
    RCC->PLLCFGR = RCC_PLLCFGR_PLL1RGE | RCC_PLLCFGR_DIVP1EN;
    RCC->PLL1DIVR = 66u | (9u << RCC_PLL1DIVR_Q1_Pos) | (1u << RCC_PLL1DIVR_R1_Pos);
    RCC->PLL1FRACR = 1136u << RCC_PLL1FRACR_FRACN1_Pos;
    RCC->PLLCFGR |= RCC_PLLCFGR_PLL1FRACEN;
    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) {}
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_3WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_3WS) {}
    RCC->D1CFGR = RCC_D1CFGR_HPRE_DIV2 | RCC_D1CFGR_D1PPRE_DIV2;
    RCC->D2CFGR = RCC_D2CFGR_D2PPRE1_DIV2 | RCC_D2CFGR_D2PPRE2_DIV2;
    RCC->D3CFGR = RCC_D3CFGR_D3PPRE_DIV2;
    RCC->CFGR = RCC_CFGR_SW_PLL1 | RCC_CFGR_MCO1_1 | RCC_CFGR_MCO1PRE_0;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL1) {}
    RCC->D1CCIPR = RCC_D1CCIPR_CKPERSEL_1;
    RCC->D2CCIP1R = RCC_D2CCIP1R_SPI123SEL_2;
    RCC->D2CCIP2R = 0;
    SCB_EnableICache();
    SysTick_Config(core_clock_hz / 1000);

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                    RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIODEN;
    (void)RCC->AHB4ENR;
    GPIOB->BSRR = (1u << 10) | (0xe00fu << 16);
    GPIOC->BSRR = ((1u << 5) | (1u << 6) | (1u << 7)) << 16;
    for (unsigned pin : {0u, 1u, 2u, 10u, 13u, 14u, 15u})
        configure_pin(GPIOB, pin, 1);
    for (unsigned pin : {5u, 6u, 7u}) configure_pin(GPIOC, pin, 1);
    configure_pin(GPIOA, 8, 2, 0, 0, 3);
}

/* ---- configure one board pin without a peripheral abstraction layer ------ */

void configure_pin(GPIO_TypeDef* port, unsigned pin, unsigned mode,
                   unsigned alternate, unsigned pull, unsigned speed)
{
    port->AFR[pin / 8] = (port->AFR[pin / 8] & ~(15u << ((pin % 8) * 4))) |
                         (alternate << ((pin % 8) * 4));
    port->PUPDR = (port->PUPDR & ~(3u << (pin * 2))) | (pull << (pin * 2));
    port->OSPEEDR = (port->OSPEEDR & ~(3u << (pin * 2))) | (speed << (pin * 2));
    port->MODER = (port->MODER & ~(3u << (pin * 2))) | (mode << (pin * 2));
}

/* ---- halt execution with the DAC held in reset -------------------------- */

[[noreturn]] void fail()
{
    __disable_irq();
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;
    configure_pin(GPIOC, 7, 1);
    write_pin(GPIOC, 7, false);
    for (;;) __WFI();
}

/* ---- handle unexpected exceptions from the official startup table -------- */

extern "C" void Error_Handler()
{ fail(); }

/* ---- advance the board's millisecond clock ------------------------------- */

extern "C" void SysTick_Handler()
{ ++milliseconds; }

/* ---- observe the DAC's required startup delays --------------------------- */

void delay_ms(uint32_t duration)
{
    uint32_t const start(milliseconds);
    while (milliseconds - start <= duration) {}
}

/* ---- discard standard output on the minimal board ----------------------- */

extern "C" int _write(int, char const*, int count)
{ return count; }

/* ---- extend the C library heap within the linker-defined boundary -------- */

extern "C" void* _sbrk(ptrdiff_t increment)
{
    extern char _end;
    extern char _heap_limit;
    static char* next(&_end);
    if (increment < 0 || uintptr_t(increment) > uintptr_t(&_heap_limit) - uintptr_t(next)) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    char* result(next);
    next += increment;
    return result;
}

/* ---- reject unavailable standard input ---------------------------------- */
extern "C" int _read(int, char*, int) { errno = ENOSYS; return -1; }
/* ---- identify the diagnostic stream ------------------------------------- */
extern "C" int _fstat(int, struct stat* status) { status->st_mode = S_IFCHR; return 0; }
/* ---- identify character output ------------------------------------------ */
extern "C" int _isatty(int) { return 1; }
/* ---- provide the unused stream seek hook -------------------------------- */
extern "C" int _lseek(int, int, int) { return 0; }
/* ---- reject closing an unavailable file --------------------------------- */
extern "C" int _close(int) { errno = EBADF; return -1; }
/* ---- identify the single firmware execution context ---------------------- */
extern "C" int _getpid() { return 1; }
/* ---- reject process signals --------------------------------------------- */
extern "C" int _kill(int, int) { errno = EINVAL; return -1; }
/* ---- halt execution when the C library terminates ------------------------ */
extern "C" void _exit(int) { fail(); }
