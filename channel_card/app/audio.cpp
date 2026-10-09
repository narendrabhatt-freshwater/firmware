#include "audio.h"
#include "voice.h"
#include "stream.h"
#include "main.h"
#include "i2s.h"
#include <stdint.h>
#include <string.h>

/* ---- generate control voltage samples ----------------------------------- */

/* CH2..CH4 use indices 1..3; CH1 carries the note-bank mix. */
static int32_t dc_level_tgt[4] = {0, 0, 0, 0}; /* target Q31 sample value */
static int32_t dc_level_now[4] = {0, 0, 0, 0}; /* slewed current value */
/* Limit each sample step to reduce DAC interpolation-filter ringing on
 * the control lines. Traversing -0.5 to +0.5 full scale takes about 85 ms
 * at AUDIO_SAMPLE_RATE_HZ = 48000. */
#define DC_SLEW_STEP (1L << 19)

static int32_t Audio_DC_NextSample(uint8_t ch)
{
    int32_t now = dc_level_now[ch];
    int32_t tgt = dc_level_tgt[ch];
    if (now < tgt) {
        now += DC_SLEW_STEP;
        if (now > tgt) {
            now = tgt;
        }
    } else if (now > tgt) {
        now -= DC_SLEW_STEP;
        if (now < tgt) {
            now = tgt;
        }
    }
    dc_level_now[ch] = now;
    return now;
}

/* ---- calibrate control voltage outputs ---------------------------------- */

/* Symmetric DC range cap, in % of DAC full scale. The analog chain rides on
 * VMID, so headroom is asymmetric — the smaller (negative-going) side sets
 * the usable range and BOTH polarities are capped to it, keeping the output
 * symmetric about the VMID center. Positive headroom above the cap is
 * intentionally unused. */
static uint8_t dc_fs_limit_pct = 50;

/* Per-channel sign inversion (bit = channel index). The analog conditioning
 * stages are inverting, so this makes a positive requested level produce a
 * POSITIVE voltage at the final control point regardless of chain sign. */
static uint8_t dc_invert_mask = 0;

/* Per-channel calibrated zero: the dc-code (in % units) at which the ANALOG
 * output crosses 0 V (the VMID offset seen through the conditioning stage).
 * With zero Z set, requested percent p maps to:  eff = Z + p*(100-|Z|)/100
 *   p = 0    -> true 0 V at the output
 *   p = +100 -> full reach on the wide side
 *   p = -100 -> the voltage MIRROR of +100 around the new zero
 * so the control is symmetric in physical volts about 0 V. */
/* Board calibration (measured 2026-07-13): output crosses 0 V at these
 * codes — CH2: +32, CH3: +22, CH4: +32. Re-measure if VMID or the
 * conditioning stages change. */
static int8_t dc_zero_pct[4] = {0, 32, 22, 32};

/* Fine zero trim in 0.01% units (±9.99%), added on top of dc_zero_pct.
 * Replaces an analog trimmer: with the 50% range cap one step ≈ 0.13 mV at
 * the output — nulls diff-amp resistor-tolerance residuals digitally. */
static int16_t dc_trim_x100[4] = {0, 0, 0, 0};

/* ---- convert a dc level to a sample target ------------------------------ */

static int32_t DC_PctToTarget(uint8_t idx, int8_t percent)
{
    int32_t p = ((dc_invert_mask >> idx) & 1u) ? -percent : percent;
    int32_t z = dc_zero_pct[idx];
    int32_t az = (z < 0) ? -z : z;
    /* effective percent × 100 for precision: z*100 + p*(100-|z|)  (|..| ≤ 10000) */
    int32_t eff_x100 = z * 100 + p * (100 - az) + dc_trim_x100[idx];
    return (int32_t)(((int64_t)0x7FFFFFFF * eff_x100 * dc_fs_limit_pct) / 1000000);
}

/* ---- configure control voltage outputs ---------------------------------- */

void Audio_SetDCLevel(uint8_t channel, int8_t percent)
{
    if (channel >= 2 && channel <= 4 && percent >= -100 && percent <= 100) {
        uint8_t idx = channel - 1;
        /* The generator slews toward this target by at most DC_SLEW_STEP. */
        dc_level_tgt[idx] = DC_PctToTarget(idx, percent);
    }
}

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
 * Retain the 1 ms audio refill cadence independently of CDC transfer timing.
 * Each DMA half contains AUDIO_SAMPLE_RATE_HZ/1000 frames.
 */
#define AUDIO_I2S_HALF_FRAMES (AUDIO_SAMPLE_RATE_HZ / 1000u)
#define AUDIO_I2S_BUF_FRAMES (AUDIO_I2S_HALF_FRAMES * 2u)
#define AUDIO_I2S_BUF_SIZE (AUDIO_I2S_BUF_FRAMES * 2u) /* × 2 for L+R, 32-bit words */
static_assert((AUDIO_SAMPLE_RATE_HZ % 1000u) == 0u,
              "I2S half must be an integer millisecond at AUDIO_SAMPLE_RATE_HZ");

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
 * explicitly in Audio_StartPlayback() before DMA starts.
 */
/* I2S1 DMA buffer for channels 1+2 */
static int32_t i2s1_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

/* I2S2 DMA buffer for control voltage channels 3+4. */
static int32_t i2s2_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

static volatile uint8_t i2s_started = 0;
static volatile uint8_t s_cpu_load_probe = 0u;

/* ---- drive the audio load probe ----------------------------------------- */

static inline void Audio_CpuLoadProbeBusy(uint8_t busy)
{
    if (s_cpu_load_probe != 0u) {
        /* Direct BSRR keeps probe overhead deterministic. Low=busy, high=idle. */
        LED_Y_GPIO_Port->BSRR = busy != 0u ? (uint32_t)LED_Y_Pin << 16u : (uint32_t)LED_Y_Pin;
    }
}

/* I2S1 sample fill runs in the DMA half/full ISR. Deferring refill to the
 * main loop can miss the 1 ms deadline when USB or RS485 work runs first,
 * causing DMA to replay the previous half. Eight voices at 48 kHz fit the
 * callback budget. */
static volatile uint8_t i2s1_fill_busy = 0;
volatile uint32_t g_i2s1_fill_late = 0;

static void Audio_FillDC(int32_t *buf, uint32_t num_frames, uint8_t chL, uint8_t chR);
static void Audio_FillDCSlot(int32_t *buf, uint32_t num_frames, uint8_t ch, uint8_t slot);
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames, uint8_t slot);
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames);
static HAL_StatusTypeDef I2S2_Start(void);

/* ---- fill control voltage buffers --------------------------------------- */

/**
 * @brief Fill interleaved samples for two control voltage output channels.
 * @param  buf         Pointer to stereo buffer (interleaved L+R, 32-bit)
 * @param  num_frames  Number of stereo frames to generate
 */
static void Audio_FillDC(int32_t *buf, uint32_t num_frames, uint8_t chL, uint8_t chR)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2] = Audio_DC_NextSample(chL);
        buf[i * 2 + 1] = Audio_DC_NextSample(chR);
    }
}

/**
 * @brief  Fill ONE slot (0 = L, 1 = R) of an interleaved stereo buffer with
 *         calibrated DC output, leaving the other slot untouched.
 *         CH2 occupies the right slot; the note-bank mix owns the left slot.
 */
static void Audio_FillDCSlot(int32_t *buf, uint32_t num_frames, uint8_t ch, uint8_t slot)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2 + slot] = Audio_DC_NextSample(ch);
    }
}

/* ---- fill the channel audio mix ----------------------------------------- */

/**
 * @brief Fill one slot with the mixed n0–n7 note bank during each DMA refill.
 */
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames, uint8_t slot)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        buf[i * 2 + slot] = NoteBank_NextSample();
    }
}

/** CH1 left: always SAMPLE note-bank mix (BODY fills rings, not I2S). */
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames)
{
    Audio_FillCh1NoteBankSlot(buf, num_frames, 0);
}

/* ---- start playback transfers ------------------------------------------- */

/**
 * @brief  Start standalone I2S playback without waiting for USB.
 */
void Audio_StartPlayback(void)
{
    if (!i2s_started) {
        memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
        memset(i2s2_tx_buf, 0, sizeof(i2s2_tx_buf));
        Audio_FillDCSlot(i2s1_tx_buf, AUDIO_I2S_BUF_FRAMES, 1, 1);
        HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)i2s1_tx_buf, AUDIO_I2S_BUF_SIZE);
#if AUDIO_USE_I2S2
        HAL_Delay(2);
        I2S2_Start();
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

void Audio_I2S2_Pump(void)
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
            s = Audio_DC_NextSample(2); /* CH3 on I2S2 left slot */
        } else {
            s = Audio_DC_NextSample(3); /* CH4 on I2S2 right slot */
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

static void I2S2_PumpTimerInit(void)
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
static HAL_StatusTypeDef I2S2_Start(void)
{
    Audio_FillDC(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
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
        I2S2_PumpTimerInit();
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
        I2S2_PumpTimerInit();
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
static void Audio_I2S1_FillHalf(uint8_t half)
{
    int32_t *buf = (half == 0u) ? &i2s1_tx_buf[0] : &i2s1_tx_buf[AUDIO_I2S_BUF_FRAMES];
    const uint32_t frames = AUDIO_I2S_BUF_FRAMES / 2u;

    if (i2s1_fill_busy != 0u) {
        g_i2s1_fill_late++;
        /* Re-entering a DMA half refill means output timing is already corrupt. */
        Error_Handler();
    }
    i2s1_fill_busy = 1u;
    NoteBank_VmBoundaryBegin();
    Audio_FillDCSlot(buf, frames, 1, 1);
    Audio_RefillCh1Slot(buf, frames);
    NoteBank_VmBoundaryEnd();
    i2s1_fill_busy = 0u;
}

/* ---- query audio diagnostics -------------------------------------------- */

uint32_t Audio_Bridge_UsbDropCount(void)
{
    return StreamRing_DropCount();
}

void Audio_Bridge_UsbDropCountClear(void)
{
    StreamRing_StatsClear();
    NoteBank_HoldCountClear();
    g_i2s1_fill_late = 0u;
}

uint32_t Audio_Bridge_MaxFill(void)
{
    return StreamRing_MaxFill();
}

uint32_t Audio_Bridge_FillLate(void)
{
    return g_i2s1_fill_late;
}

void Audio_Bridge_CpuLoadProbeSet(uint8_t enabled)
{
    s_cpu_load_probe = enabled != 0u ? 1u : 0u;
    LED_Y_GPIO_Port->BSRR =
        s_cpu_load_probe != 0u ? (uint32_t)LED_Y_Pin : (uint32_t)LED_Y_Pin << 16u;
}

uint8_t Audio_Bridge_CpuLoadProbeGet(void)
{
    return s_cpu_load_probe;
}

/* ---- service i2s dma callbacks ------------------------------------------ */

/**
 * @brief  I2S1 DMA half transfer complete callback.
 *         DMA now plays the second half → refill the first.
 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI1) {
        Audio_CpuLoadProbeBusy(1u);
        Audio_I2S1_FillHalf(0u);
        Audio_CpuLoadProbeBusy(0u);
    } else if (hi2s->Instance == SPI2) {
        Audio_FillDC(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
    }
}

/**
 * @brief  I2S DMA full transfer complete callback.
 *         SPI1: DMA wrapped → refill the second half.
 */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI1) {
        Audio_CpuLoadProbeBusy(1u);
        Audio_I2S1_FillHalf(1u);
        Audio_CpuLoadProbeBusy(0u);
    } else if (hi2s->Instance == SPI2) {
#if AUDIO_I2S2_IT
        /* IT mode: no half-complete events — refill the WHOLE buffer here,
         * then re-arm the interrupt transfer (self-sustaining chain). */
        Audio_FillDC(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
#else
        Audio_FillDC(&i2s2_tx_buf[AUDIO_I2S_BUF_FRAMES], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
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
