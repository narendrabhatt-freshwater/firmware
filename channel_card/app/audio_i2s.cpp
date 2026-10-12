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

#include "audio.h"
#include "audio_output.h"
#include "board.h"
#include "main.h"
#include "i2s.h"
#include <stdint.h>
#include <string.h>

/* ---- feed the audio dma buffers ----------------------------------------- */

/* Note-bank → I2S bridge for the CS4304 4-channel DAC.
 * Owns I2S DMA ring buffers, CH1 note-bank refill,
 * the TIM7 I2S2 underrun pump, and the LED_Y DMA-load scope probe. */

/*
 * I2S DMA buffer sizing:
 * I2S is configured for 32-bit data, stereo (L+R), at AUDIO_SAMPLE_RATE_HZ.
 * Each I2S frame = 2 × 32-bit words (L + R) = 8 bytes
 * DMA buffer is double-buffered: half/full IRQs each refill one half.
 *
 * Default to 48 frames per half. This device setting is independent of
 * the application script interval; e.g. 96 needs no application change.
 */
#ifndef AUDIO_I2S_HALF_FRAMES
#define AUDIO_I2S_HALF_FRAMES 48u
#endif
#define AUDIO_I2S_BUF_FRAMES (AUDIO_I2S_HALF_FRAMES * 2u)
#define AUDIO_I2S_BUF_SIZE (AUDIO_I2S_BUF_FRAMES * 2u) /* × 2 for L+R, 32-bit words */
static_assert(AUDIO_I2S_HALF_FRAMES > 0u && AUDIO_I2S_BUF_SIZE <= 65535u,
              "DMA transfer must fit the HAL uint16 size");

static_assert(AUDIO_SAMPLE_RATE_HZ == 48000u,
              "This board clock and DAC configuration require 48 kHz");

/* I2S2 (CH3/CH4) enabled. Slave TX on SPI2 requires two workarounds:
 *  1. UDR wedge — the H7 slave halts on underrun until the flag is
 *     cleared; the TIM7 pump clears it every tick.
 *  2. IOSWP — the H7 slave transmits on MISO, but the board wires PC1
 *     (MOSI) to the DAC's SDIN2; CFG2.IOSWP swaps them internally. */
#define AUDIO_USE_I2S2 1

/* 0 = DMA feed (preferred): guaranteed sample ordering → stable CH3/CH4
 *     left/right assignment and no pump-parity slips (which caused spikes
 *     and CH3/CH4 swapping in pump mode). TIM7 stays on as a UDR guard.
 * 1 = TIM7 FIFO pump (fallback if DMA misbehaves). */
#define AUDIO_I2S2_IT 0

/*
 * IMPORTANT: DMA1 (D2 domain) cannot access DTCM RAM where .bss lives.
 * Both buffers are placed in AXI SRAM (RAM_D1) via the .dma_buffer section
 * (see STM32H725xG_flash.ld). Section is NOLOAD: buffers are cleared
 * explicitly in start_audio_playback() before DMA starts.
 */
/* I2S1 DMA buffer for channels 1+2 */
static int32_t i2s1_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

/* I2S2 DMA buffer for control voltage channels 3+4. */
static int32_t i2s2_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

static volatile uint8_t i2s_started = 0;

/* I2S1 sample fill runs in the DMA half/full ISR. Deferring refill to the
 * main loop can miss the 1 ms deadline when USB or RS485 work runs first,
 * causing DMA to replay the previous half. Eight voices at 48 kHz fit the
 * callback budget. */
static volatile uint8_t i2s1_fill_busy = 0;
volatile uint32_t g_i2s1_fill_late = 0;

static HAL_StatusTypeDef start_i2s2_playback(void);
static uint32_t read_audio_refill_overlap_count(void);
static void clear_audio_output_statistics(void);
static const AudioOutputDiagnostics output_diagnostics = {
    read_audio_refill_overlap_count, clear_audio_output_statistics
};

/* ---- start playback transfers ------------------------------------------- */

/**
 * @brief  Start standalone I2S playback without waiting for USB.
 */
void start_audio_playback(void)
{
    set_audio_output_diagnostics(&output_diagnostics);
    if (!i2s_started) {
        memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
        memset(i2s2_tx_buf, 0, sizeof(i2s2_tx_buf));
        render_control_voltage_slot(i2s1_tx_buf, AUDIO_I2S_BUF_FRAMES, 1, 1);
        HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)i2s1_tx_buf, AUDIO_I2S_BUF_SIZE);
#if AUDIO_USE_I2S2
        HAL_Delay(2);
        start_i2s2_playback();
#endif
        i2s_started = 1;
    }
}

/* TIM7 guards I2S2 against underrun in DMA mode. The optional interrupt
 * mode also feeds the FIFO from this timer. */
volatile uint32_t g_pump_words = 0;      /* debugger: must keep rising */
volatile uint32_t g_i2s2_udr_clears = 0; /* UDR wedge recoveries */
static volatile uint8_t i2s2_pump_on = 0;
#if AUDIO_I2S2_IT
static uint8_t pump_slot = 0; /* 0 = CH3 (L), 1 = CH4 (R) */
#endif

/* ---- service the secondary i2s output ----------------------------------- */

void service_i2s2_underrun(void)
{
    if (!i2s2_pump_on) {
        return;
    }
    /* A slave-TX underrun (UDR) wedges the transmitter — it stops consuming
     * the FIFO until the flag is cleared (observed: SR=0x20, FIFO full, no
     * draining). Clear it every tick so transmission always resumes. */
    if ((SPI2->SR & SPI_SR_UDR) != 0u) {
        SPI2->IFCR = SPI_IFCR_UDRC;
        g_i2s2_udr_clears++;
    }
    if ((SPI2->SR & SPI_SR_TIFRE) != 0u) {
        SPI2->IFCR = SPI_IFCR_TIFREC;
    }

#if AUDIO_I2S2_IT
    /* Feed CH3/CH4 directly from the timer in interrupt mode. */
    if (i2s2_pump_on && ((SPI2->SR & SPI_SR_TXP) != 0u)) {
        int32_t s;
        if (pump_slot == 0) {
            s = render_next_control_voltage_sample(2); /* CH3 on I2S2 left slot */
        } else {
            s = render_next_control_voltage_sample(3); /* CH4 on I2S2 right slot */
        }
        SPI2->TXDR = (uint32_t)s;
        pump_slot ^= 1u;
        g_pump_words++;
    }
#endif
    /* DMA mode: feeding is done by DMA + half/complete callbacks; this tick
     * is only the UDR guard above. */
}

/* ---- configure the secondary i2s timer ---------------------------------- */

static void init_i2s2_underrun_timer(void)
{
    static uint8_t inited = 0;
    if (inited) {
        return;
    }
    inited = 1;
    __HAL_RCC_TIM7_CLK_ENABLE();
    TIM7->PSC = 274; /* 275 MHz / 275 = 1 MHz            */
    TIM7->ARR = 9;   /* 1 MHz / 10 = 100 kHz tick        */
    TIM7->DIER = TIM_DIER_UIE;
    /* USB OTG (priority 0) preempts this tick. This tick preempts the I2S1 DMA
     * fill (priority 2) so a long mix cannot wedge the I2S2 slave. */
    HAL_NVIC_SetPriority(TIM7_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
}

/* ---- start the secondary i2s peripheral --------------------------------- */

/* Prefill CH3/CH4 with slewed DC samples before starting the transfer. */
static HAL_StatusTypeDef start_i2s2_playback(void)
{
    render_control_voltage_channels(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
#if AUDIO_I2S2_IT
    {
        /* H7 SPI: a SLAVE transmits on MISO, but the board wires PC1 = MOSI to
         * the DAC's SDIN2 (the master-mode SDO pin). IOSWP swaps MISO/MOSI
         * inside the peripheral so the slave's data comes out on PC1.
         * CFG2 is only writable while SPE = 0. */
        CLEAR_BIT(SPI2->CR1, SPI_CR1_SPE);
        SET_BIT(SPI2->CFG2, SPI_CFG2_IOSWP);

        HAL_StatusTypeDef st =
            HAL_I2S_Transmit_IT(&hi2s2, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);
        /* ROOT CAUSE of the CH3/CH4 silence: a transient UDR (slave underrun)
         * at stream start makes the stock HAL abort the whole transfer.
         * UDR is benign here (one repeated sample) — clear it and mask the
         * interrupt so HAL never sees it.
         * FRE must be masked too: HAL enables it for slaves but its IRQ handler
         * never services/clears it in TX state → interrupt storm if it sets. */
        __HAL_I2S_CLEAR_UDRFLAG(&hi2s2);
        __HAL_I2S_CLEAR_TIFREFLAG(&hi2s2);
        /* Keep HAL fully dormant (TXP too): the TIM7 pump owns the FIFO. */
        __HAL_I2S_DISABLE_IT(&hi2s2, (I2S_IT_TXP | I2S_IT_UDR | I2S_IT_FRE));
        pump_slot = 0;
        init_i2s2_underrun_timer();
        i2s2_pump_on = 1;
        return st;
    }
#else
    {
        /* IOSWP: H7 slave transmits on MISO; board wires PC1 = MOSI to SDIN2.
         * Swap internally. CFG2 writable only while SPE = 0. */
        CLEAR_BIT(SPI2->CR1, SPI_CR1_SPE);
        SET_BIT(SPI2->CFG2, SPI_CFG2_IOSWP);

        HAL_StatusTypeDef st =
            HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);

        /* Clear the inevitable start-up underrun and mask error interrupts so
         * HAL can never abort the transfer. TIM7 keeps running as a fast UDR
         * guard (clears the flag within 10 µs so the slave never wedges). */
        __HAL_I2S_CLEAR_UDRFLAG(&hi2s2);
        __HAL_I2S_CLEAR_TIFREFLAG(&hi2s2);
        __HAL_I2S_DISABLE_IT(&hi2s2, (I2S_IT_TXP | I2S_IT_UDR | I2S_IT_FRE));
        init_i2s2_underrun_timer();
        i2s2_pump_on = 1;
        return st;
    }
#endif
}

/* ---- refill one audio dma half ------------------------------------------ */

/**
 * Fill one free half of i2s1_tx_buf (CH2 right + CH1 left).
 * Called from the I2S1 DMA half/full ISR.
 */
static void refill_i2s1_buffer_half(uint8_t half)
{
    int32_t *buf = (half == 0u) ? &i2s1_tx_buf[0] : &i2s1_tx_buf[AUDIO_I2S_BUF_FRAMES];
    const uint32_t frames = AUDIO_I2S_BUF_FRAMES / 2u;

    if (i2s1_fill_busy != 0u) {
        g_i2s1_fill_late++;
        /* Re-entering a DMA half refill means output timing is already corrupt. */
        Error_Handler();
    }
    i2s1_fill_busy = 1u;
    render_dac_audio_frames(buf, frames);
    i2s1_fill_busy = 0u;
}

/* ---- service i2s dma callbacks ------------------------------------------ */

/**
 * @brief  I2S1 DMA half transfer complete callback.
 *         DMA now plays the second half → refill the first.
 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI1) {
        refill_i2s1_buffer_half(0u);
    } else if (hi2s->Instance == SPI2) {
        render_control_voltage_channels(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
    }
}

/**
 * @brief  I2S DMA full transfer complete callback.
 *         SPI1: DMA wrapped → refill the second half.
 */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI1) {
        refill_i2s1_buffer_half(1u);
    } else if (hi2s->Instance == SPI2) {
#if AUDIO_I2S2_IT
        /* IT mode: no half-complete events — refill the WHOLE buffer here,
         * then re-arm the interrupt transfer (self-sustaining chain). */
        render_control_voltage_channels(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
#else
        render_control_voltage_channels(&i2s2_tx_buf[AUDIO_I2S_BUF_FRAMES],
                                        AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
#endif
#if AUDIO_I2S2_IT
        HAL_I2S_Transmit_IT(&hi2s2, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);
        __HAL_I2S_CLEAR_UDRFLAG(&hi2s2);
        __HAL_I2S_CLEAR_TIFREFLAG(&hi2s2);
        __HAL_I2S_DISABLE_IT(&hi2s2, (I2S_IT_UDR | I2S_IT_FRE));
#endif
    }
}

/* Count of self-healing restarts after an I2S error (debugger-visible) */
volatile uint32_t g_i2s2_err_restarts = 0;

/**
 * @brief  I2S error callback — self-heal I2S2: if any error still aborts
 *         the interrupt transfer, clear the flag and re-arm immediately.
 */
void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        g_i2s2_err_restarts++;
        __HAL_I2S_CLEAR_UDRFLAG(hi2s);
        __HAL_I2S_CLEAR_TIFREFLAG(hi2s);
#if AUDIO_I2S2_IT
        HAL_I2S_Transmit_IT(hi2s, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);
#else
        HAL_I2S_Transmit_DMA(hi2s, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);
#endif
        __HAL_I2S_DISABLE_IT(hi2s, (I2S_IT_UDR | I2S_IT_FRE));
    }
}

/* ---- report output-driver diagnostics ----------------------------------- */

static uint32_t read_audio_refill_overlap_count(void)
{
    return g_i2s1_fill_late;
}

static void clear_audio_output_statistics(void)
{
    g_i2s1_fill_late = 0u;
}
