#ifndef MINIMAL_BOARD_H
#define MINIMAL_BOARD_H

#include "stm32h725xx.h"
#include <cstdint>
#include <vector>

/* ---- MCU clocks and timing --------------------------------------------- */

constexpr uint32_t core_clock_hz(550000000u);
constexpr uint32_t peripheral_clock_hz(core_clock_hz / 4);

void init_board();
[[noreturn]] void fail();
void delay_ms(uint32_t duration);

/* ---- GPIO configuration ------------------------------------------------ */

void configure_pin(GPIO_TypeDef* port, unsigned pin, unsigned mode,
                   unsigned alternate = 0, unsigned pull = 0, unsigned speed = 0);

/* ---- set or clear one GPIO output ---------------------------------------- */

inline void write_pin(GPIO_TypeDef* port, unsigned pin, bool high)
{
    port->BSRR = high ? (1u << pin) : (1u << (pin + 16));
}

/* ---- DAC and audio DMA ------------------------------------------------- */

void init_dac();
void init_audio();
void start_audio();

/* ---- RS485 serial I/O -------------------------------------------------- */

#ifndef RS485_BAUD
#define RS485_BAUD 921600u
#endif

void init_serial();
bool process_serial(std::vector<uint8_t>& bytes);

#endif
