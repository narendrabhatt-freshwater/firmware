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
#include "audio_buffers.h"
#include <hardware.h>
#include <interrupt.h>
#include "audio_output.h"
#include "board.h"
#include "main.h"
#include "i2s.h"
#include <stdint.h>
#include <string.h>

/* ---- feed the audio dma buffers ----------------------------------------- */

/* Note-bank → I2S bridge for the CS4304 4-channel DAC.
 * Owns the DMA buffer rings and the TIM7 I2S2 underrun guard. */

// Each DMA transfer is one 48-frame stereo block. The two hardware memory
// addresses advance through the BUF_COUNT blocks without copying samples.
static_assert(BUF_COUNT >= 2, "DMA needs a playing block and a following block");
static_assert(AUDIO_BUFFER_FRAMES == AUDIO_PROCESSING_INTERVAL_SAMPLES,
              "DMA buffers hold exactly one application call");
static_assert(AUDIO_BUFFER_CHANNELS == 2 && AUDIO_BUFFER_WORDS <= 65535u,
              "I2S needs stereo blocks fitting the DMA transfer count");
static_assert(AUDIO_SAMPLE_RATE_HZ == 48000u, "The board clock requires 48 kHz");

static volatile uint8_t i2s_started = 0;

volatile uint32_t g_i2s1_fill_late = 0;
volatile uint32_t g_i2s2_fill_late = 0;

static HAL_StatusTypeDef start_i2s2_playback(void);
static uint32_t read_audio_refill_overlap_count(void);
static void clear_audio_output_statistics(void);
static const AudioOutputDiagnostics output_diagnostics = {
    read_audio_refill_overlap_count, clear_audio_output_statistics
};

static void dma_block_completed(DMA_HandleTypeDef *dma);
static void audio_dma_error(DMA_HandleTypeDef *dma);

static HAL_StatusTypeDef start_audio_dma(I2S_HandleTypeDef &i2s, AudioBuffers &buffers)
{
    DMA_HandleTypeDef *dma = i2s.hdmatx;
    auto *stream = static_cast<DMA_Stream_TypeDef *>(dma->Instance);
    const unsigned first = buffers.read_index % BUF_COUNT;
    if ((__atomic_load_n(&buffers.write_index, __ATOMIC_ACQUIRE) + 2u * BUF_COUNT -
         buffers.read_index) % (2u * BUF_COUNT) < 2u) return HAL_ERROR;
    dma->XferCpltCallback = dma_block_completed;
    dma->XferM1CpltCallback = dma_block_completed;
    dma->XferHalfCpltCallback = nullptr;
    dma->XferM1HalfCpltCallback = nullptr;
    dma->XferErrorCallback = audio_dma_error;
    CLEAR_BIT(stream->CR, DMA_SxCR_CT);
    __DMB();
    const HAL_StatusTypeDef status = HAL_DMAEx_MultiBufferStart_IT(
        dma, (uintptr_t)buffers.samples[first], (uintptr_t)&i2s.Instance->TXDR,
        (uintptr_t)buffers.samples[(first + 1u) % BUF_COUNT], AUDIO_BUFFER_WORDS);
    if (status != HAL_OK) return status;
    i2s.pTxBuffPtr = (uint16_t *)buffers.samples[first];
    i2s.TxXferSize = i2s.TxXferCount = AUDIO_BUFFER_WORDS;
    i2s.ErrorCode = HAL_I2S_ERROR_NONE;
    i2s.State = HAL_I2S_STATE_BUSY_TX;
    SET_BIT(i2s.Instance->CFG1, SPI_CFG1_TXDMAEN);
    __HAL_I2S_ENABLE(&i2s);
    SET_BIT(i2s.Instance->CR1, SPI_CR1_CSTART);
    return HAL_OK;
}

/* ---- start playback transfers ------------------------------------------- */

/**
 * @brief  Start standalone I2S playback without waiting for USB.
 */
void start_audio_playback(void)
{
    set_audio_output_diagnostics(&output_diagnostics);
    if (!i2s_started) {
        audio_output = {};
        control_voltage_output = {};
        for (unsigned i = 0; i < BUF_COUNT; ++i) {
            render_control_voltage_slot(audio_output.samples[i], AUDIO_BUFFER_FRAMES, 1, 1);
            render_control_voltage_channels(control_voltage_output.samples[i], AUDIO_BUFFER_FRAMES, 2, 3);
        }
        audio_output.write_index = control_voltage_output.write_index = BUF_COUNT;
        i2s_started = 1;
        // Both outputs are primed before the clock starts. Preserve master-first
        // startup, then enter main without sleeping through a refill deadline.
        if (start_audio_dma(hi2s1, audio_output) != HAL_OK ||
            start_i2s2_playback() != HAL_OK) Error_Handler();
    }
}

/* TIM7 guards I2S2 against hardware underrun. */
volatile uint32_t g_i2s2_udr_clears = 0; /* UDR wedge recoveries */
static volatile uint8_t i2s2_pump_on = 0;

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

    /* This timer only services hardware flags; main generates audio. */
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
    /* USB OTG (priority 0) preempts this tick. This tick preempts both the
     * I2S1 completion interrupt (priority 2) and main-loop rendering. */
    HAL_NVIC_SetPriority(TIM7_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
}

/* ---- start the secondary i2s peripheral --------------------------------- */

/* Start the already primed CH3/CH4 transfer. */
static HAL_StatusTypeDef start_i2s2_playback(void)
{
    {
        /* IOSWP: H7 slave transmits on MISO; board wires PC1 = MOSI to SDIN2.
         * Swap internally. CFG2 writable only while SPE = 0. */
        CLEAR_BIT(SPI2->CR1, SPI_CR1_SPE);
        SET_BIT(SPI2->CFG2, SPI_CFG2_IOSWP);

        HAL_StatusTypeDef st =
            start_audio_dma(hi2s2, control_voltage_output);

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
}

/* ---- publish completion interrupts and refill from main ---------------- */

static void refill_deadline_missed(unsigned port)
{
    if (port == 0) ++g_i2s1_fill_late;
    else ++g_i2s2_fill_late;
    Error_Handler();
}

static void dma_block_completed(DMA_HandleTypeDef *dma)
{
    auto *i2s = static_cast<I2S_HandleTypeDef *>(dma->Parent);
    const unsigned port = i2s->Instance == SPI1 ? 0u : 1u;
    AudioBuffers &buffers = port == 0 ? audio_output : control_voltage_output;
    auto *stream = static_cast<DMA_Stream_TypeDef *>(dma->Instance);
    const bool playing_memory1 = (stream->CR & DMA_SxCR_CT) != 0;
    const unsigned next = (buffers.read_index + 1u) % (2u * BUF_COUNT);
    // Hardware has already switched to the next block. Check its address before
    // freeing the old one, so a missed interrupt cannot free the playing block.
    if ((playing_memory1 ? stream->M1AR : stream->M0AR) != (uintptr_t)buffers.samples[next % BUF_COUNT]) {
        refill_deadline_missed(port);
        return;
    }
    __atomic_store_n(&buffers.read_index, next, __ATOMIC_RELEASE);
    if (__atomic_load_n(&buffers.write_index, __ATOMIC_ACQUIRE) == next) {
        refill_deadline_missed(port);
        return;
    }
    // Only update the inactive address. Main refills freed slots before DMA
    // reaches them again; the current block continues without a transfer restart.
    HAL_DMAEx_ChangeMemory(dma, (uintptr_t)buffers.samples[(next + 1u) % BUF_COUNT],
                          playing_memory1 ? MEMORY0 : MEMORY1);
    signal_interrupt();
}

static void audio_dma_error(DMA_HandleTypeDef *dma)
{
    auto *i2s = static_cast<I2S_HandleTypeDef *>(dma->Parent);
    CLEAR_BIT(i2s->Instance->CFG1, SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
    i2s->TxXferCount = i2s->RxXferCount = 0;
    i2s->State = HAL_I2S_STATE_READY;
    i2s->ErrorCode |= HAL_I2S_ERROR_DMA;
    HAL_I2S_ErrorCallback(i2s);
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
        if (start_audio_dma(*hi2s, control_voltage_output) != HAL_OK) Error_Handler();
        __HAL_I2S_DISABLE_IT(hi2s, (I2S_IT_UDR | I2S_IT_FRE));
    }
}

/* ---- report output-driver diagnostics ----------------------------------- */

static uint32_t read_audio_refill_overlap_count(void)
{
    return g_i2s1_fill_late + g_i2s2_fill_late;
}

static void clear_audio_output_statistics(void)
{
    g_i2s1_fill_late = g_i2s2_fill_late = 0u;
}
