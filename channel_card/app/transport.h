/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef CHANNEL_TRANSPORT_H
#define CHANNEL_TRANSPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Link exactly one backend. All calls are main-loop only unless documented.
 * Byte order is preserved; physical packet/read boundaries are not messages. */
/* ---- configure console timing ------------------------------------------- */

/* Fallback for direct builds. Set BAUDRATE in app/Makefile for normal builds. */
#ifndef FW_RS485_BAUD
#define FW_RS485_BAUD 921600u
#endif

#if FW_RS485_BAUD <= 0
#error "FW_RS485_BAUD must be positive"
#endif

/* 8N1: one start bit, eight data bits, one stop bit. Round up the wire
 * time and allow two milliseconds for HAL tick granularity and software.
 * Do not cap below the wire time: that truncates replies at slower rates. */
static inline uint32_t calculate_rs485_transmit_timeout_ms(uint32_t nbytes, uint32_t baud)
{
    return 2u + (uint32_t)(((uint64_t)nbytes * 10000u + baud - 1u) / baud);
}

/* ---- transfer console bytes --------------------------------------------- */

/* Replaceable byte transport for the RS485 console. */

/** Start reception after platform initialization. Call once at cold start. */
void init_rs485_receiver(void);

/** Discard queued RX bytes and latched receive errors. Main-loop only.
 * Keeps baud configuration, TX state and lifetime drop counters intact. */
void clear_rs485_receive_state(void);

/** Pop one buffered byte. Returns 1 and writes *out if a byte was
 * available, 0 if the buffer is empty. */
uint8_t read_rs485_byte(uint8_t *out);

/** Bytes lost since boot to hardware overrun or a full ring buffer.
 * Non-zero means a console line was corrupted — never ignore it. */
uint32_t get_rs485_dropped_byte_count(void);

/* ---- share the rs485 bus ------------------------------------------------ */

/* Tri-state the driver-enable and transmit pins so another card can talk. */
void release_rs485_bus(void);
/* Take the bus for one reply; pair every call with release_rs485_bus(). */
void acquire_rs485_bus(void);
/* Non-zero while no card is driving the bus. */
uint8_t is_rs485_bus_free(void);
uint32_t get_rs485_baud_rate(void);

/* Start borrows data until poll returns complete/error or abort returns.
 * Return 0 on start, -1 on rejection. One outstanding transmission.
 * poll: 0 pending, 1 complete (UART final stop bit / host queue drained),
 * -1 error. Idle polling returns 1. Neither completion nor abort acknowledges
 * remote delivery.
 * abort stops further reads of the borrowed buffer; already sent bytes cannot
 * be recalled. Caller acquires/releases the bus on all completion paths. */
int start_rs485_transmit(const uint8_t *data, uint16_t size);
int poll_rs485_transmit(void);
void abort_rs485_transmit(void);

/* Shared application service: bounded wait with USB protocol servicing.
 * Returns 0 on completion, -1 on start/failure/timeout (milliseconds). */
int write_rs485_bytes(const uint8_t *data, uint16_t size, uint32_t timeout_ms);

/* USB reads return 0..size bytes, never block, and never imply frame alignment.
 * Writes copy all bytes into the queue or return 0 without accepting any;
 * success means queued, not delivered. Free capacity is advisory.
 * Connection generation changes discard old transport bytes and invalidate
 * protocol state. STM32 reset/configuration/DTR/disconnect advance it;
 * suspend/resume only change readiness. Fault blocks receive until an existing
 * lifecycle reset; queued replies may still drain. Simulator PTY open/close/
 * flush maps to generation changes, not to USB endpoint emulation. */
/* ---- control usb transfers ---------------------------------------------- */

void init_usb_device(void);
uint32_t read_usb_bytes(uint8_t *dst, uint32_t size);
uint8_t write_usb_bytes(const uint8_t *src, uint32_t size);
uint32_t get_usb_transmit_free_bytes(void);
uint32_t get_usb_connection_generation(void);
uint8_t is_usb_connected(void);
void fault_usb_connection(void);
void format_usb_device_status(char *text, uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
