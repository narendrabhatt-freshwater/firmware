#ifndef CHANNEL_APP_CHANNEL_H
#define CHANNEL_APP_CHANNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CS4304_HandleTypeDef CS4304_HandleTypeDef;

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
static inline uint32_t fw_rs485_tx_deadline_ms(uint32_t nbytes, uint32_t baud)
{
    return 2u + (uint32_t)(((uint64_t)nbytes * 10000u + baud - 1u) / baud);
}

/* ---- control the status leds -------------------------------------------- */

void ChannelLed_Init(void);
int ChannelLed_Set(float red, float green, float blue, float brightness);
void ChannelLed_Task(void);

/* ---- transfer console bytes --------------------------------------------- */

/* Interrupt-driven receive ring buffer for UART5 (RS485 console). */

/** Arm the UART5 RX interrupt. Call once after MX_UART5_Init(). */
void Uart5Rx_Init(void);

/** Discard queued RX bytes and latched receive errors. Main-loop only.
 * Keeps baud configuration, TX state and lifetime drop counters intact. */
void Uart5Rx_Clear(void);

int Uart5Rx_Transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms);

/** Pop one buffered byte. Returns 1 and writes *out if a byte was
 * available, 0 if the buffer is empty. */
uint8_t Uart5Rx_Get(uint8_t *out);

/** Bytes lost since boot to hardware overrun or a full ring buffer.
 * Non-zero means a console line was corrupted — never ignore it. */
uint32_t Uart5Rx_DroppedCount(void);

/* ---- initialize and poll the console ------------------------------------ */

/* Channel Card RS485 + USB CDC console and status LEDs.
 * One command parser serves both transports. RS485 uses card address prefixes
 * (`c:` / `*:` / bare) and tagged replies (`[C]ok` / `[C]err:<token>`).
 * Stable error tokens: syntax, range, unknown, rxdrop.
 * Bring-up: call ChannelConsole_SetDacHandle() then ChannelConsole_Init()
 * after UART5 and the DAC handle are ready. Poll from the main loop after
 * servicing the USB task. */

/**
 * @brief Bind the CS4304 handle used by gain / trim console commands.
 * @param h DAC handle owned by main (non-NULL before Init / Poll).
 */
void ChannelConsole_SetDacHandle(CS4304_HandleTypeDef *h);

/**
 * @brief Tri-state RS485 idle, apply boot defaults, and emit the ready banner.
 * @note Also initializes note filter / bank / envelope cold state.
 */
void ChannelConsole_Init(void);

/**
 * @brief Non-blocking RS485 RX drain + LED chaser step.
 * @note Call every main-loop iteration; never blocks.
 */
void ChannelConsole_Poll(void);

/**
 * @brief Run one console command line that arrived over USB CDC.
 * @param line NUL-terminated command (mutated by the parser). May be NULL
 *             (no-op). Replies are routed to CDC for the duration of the call.
 */
void Console_ExecFromUSB(char *line);

/* ---- upload compiled voice programs ------------------------------------- */

uint8_t VmUpload_IsActive(void);
int VmUpload_Begin(uint8_t voice, uint32_t nbytes);
uint32_t VmUpload_Feed(const uint8_t *data, uint32_t size);
void VmUpload_Abort(void);

#ifdef __cplusplus
}
#endif

#endif
