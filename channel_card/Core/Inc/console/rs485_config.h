#ifndef FRESHWATER_RS485_CONFIG_H
#define FRESHWATER_RS485_CONFIG_H

#include <stdint.h>

/* Channel Card default. Override per build with -DFW_RS485_BAUD=115200. */
#ifndef FW_RS485_BAUD
#define FW_RS485_BAUD 921600u
#endif

#if FW_RS485_BAUD <= 0
#error "FW_RS485_BAUD must be positive"
#endif

/* 8N1: one start bit, eight data bits, one stop bit. Round up the wire
 * time and allow two milliseconds for HAL tick granularity and software.
 * Do not cap below the wire time: that truncates replies at slower rates. */
static inline uint32_t fw_rs485_tx_deadline_ms(uint32_t nbytes, uint32_t baud)
{
    return 2u + (uint32_t)(((uint64_t)nbytes * 10000u + baud - 1u) / baud);
}

#endif
