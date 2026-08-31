/**
 ******************************************************************************
 * @file    audio_bridge.c
 * @brief   USB audio / note-bank → I2S bridge for the CS4304 4-channel DAC.
 *
 * Owns I2S DMA ring buffers, USB packet ingest, CH1 note-bank refill,
 * and the TIM7 I2S2 underrun pump. Tone/DC and CPU-load probe live in
 * audio_tone_dc.c / audio_cpuload.c.
 ******************************************************************************
 */

#include "audio_bridge.h"

#include "main.h"
#include "i2s.h"
#include "cs4304.h"
#include "audio_rate.h"
#include "audio_cpuload.h"
#include "audio_tone_dc.h"
#include "note_bank.h"
#include "stream_ring.h"

#include <string.h>

/* Bound from main via Audio_Bridge_SetDacHandle(). */
static CS4304_HandleTypeDef *s_dac;

void Audio_Bridge_SetDacHandle(CS4304_HandleTypeDef *h)
{
  s_dac = h;
}

/*
 * I2S DMA buffer sizing:
 * I2S is configured for 32-bit data, stereo (L+R), at AUDIO_SAMPLE_RATE_HZ.
 * Each I2S frame = 2 × 32-bit words (L + R) = 8 bytes
 * DMA buffer is double-buffered: half/full IRQs each refill one half.
 *
 * Full-speed iso OUT is one 1023-byte packet per 1 ms SOF. Size the DMA
 * half to AUDIO_SAMPLE_RATE_HZ/1000 frames so I2S consume and USB fill
 * share that 1 ms cadence.
 */
#define AUDIO_I2S_HALF_FRAMES (AUDIO_SAMPLE_RATE_HZ / 1000u)
#define AUDIO_I2S_BUF_FRAMES (AUDIO_I2S_HALF_FRAMES * 2u)
#define AUDIO_I2S_BUF_SIZE (AUDIO_I2S_BUF_FRAMES * 2u) /* × 2 for L+R, 32-bit words */
_Static_assert((AUDIO_SAMPLE_RATE_HZ % 1000u) == 0u,
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
 * (see STM32H725XG_FLASH.ld). Section is NOLOAD: buffers are cleared
 * explicitly in Audio_Bridge_Start() before DMA starts.
 */
/* I2S1 DMA buffer for channels 1+2 */
static int32_t i2s1_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

/* I2S2 DMA buffer for channels 3+4 (test/other functions) */
static int32_t i2s2_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

static volatile uint8_t i2s_started = 0;

void Audio_SetUSBMute(uint8_t mute) { (void)mute; }

static volatile uint8_t dma_active_half = 0; /* 0 = playing first half (write to second), 1 = playing second half (write to first) */

/* I2S1 sample fill runs in the DMA half/full ISR. Deferring refill to the
 * main loop can miss the 1 ms deadline when USB or RS485 work runs first,
 * causing DMA to replay the previous half. Eight voices at 48 kHz fit the
 * callback budget. */
static volatile uint8_t i2s1_fill_busy = 0;
volatile uint32_t g_i2s1_fill_late = 0;

static void Audio_FillTestTone(int32_t *buf, uint32_t num_frames,
                               uint8_t chL, uint8_t chR);
static void Audio_FillToneSlot(int32_t *buf, uint32_t num_frames,
                               uint8_t ch, uint8_t slot);
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames,
                                      uint8_t slot);
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames);
static HAL_StatusTypeDef I2S2_Start(void);

/** CH1 left slot plays the N0–NF note bank when any voice is active. */
static inline uint8_t Audio_Ch1NoteBankActive(void)
{
  return NoteBank_AnyActive();
}

void Audio_Bridge_Start(void)
{
  /* Clear DMA buffers */
  memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
  memset(i2s2_tx_buf, 0, sizeof(i2s2_tx_buf));

  /* Start I2S DMA transmit in circular mode */
  if (!i2s_started)
  {
    /*
     * ORDER MATTERS — MASTER FIRST: the H7 SPI/I2S slave only starts
     * shifting if the external clocks are already running when it is
     * enabled (every slave-armed-before-clocks attempt froze; the polling
     * test always ran with live clocks and always worked).
     *
     * Size: DMA is configured WORD/WORD (32-bit), so NDTR counts 32-bit
     * transfers. Pass the number of 32-bit words in the buffer — NOT ×2.
     * (×2 made circular DMA sweep 2× the buffer: half garbage output.)
     */
    HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)i2s1_tx_buf, AUDIO_I2S_BUF_SIZE);
#if AUDIO_USE_I2S2
    /* Let BCLK/WS run before enabling the slave. Busy-wait, not HAL_Delay:
     * this can run while I2S DMA IRQs are live. ~2 ms at 550 MHz. */
    for (volatile uint32_t d = 0; d < 300000u; d++)
    {
      __NOP();
    }
    I2S2_Start();
#endif

    i2s_started = 1;
  }

  /*
   * DAC gain is left at the level set by CS4304_Init (CS4304_INIT_ATTEN).
   */
}

/**
 * @brief  DeInitializes the AUDIO media low layer
 * @param  options: Reserved for future use
 */
void Audio_Bridge_Stop(void)
{
  /* Stop I2S DMA */
  HAL_I2S_DMAStop(&hi2s1);
#if AUDIO_USE_I2S2
  HAL_I2S_DMAStop(&hi2s2);
#endif
  i2s_started = 0;
}

/**
 * @brief  Handles AUDIO command.
 * @param  pbuf: Pointer to buffer of data to be sent
 * @param  size: Number of data to be sent (in bytes)
 * @param  cmd: Command opcode
 */
void Audio_Bridge_StreamStop(void)
{
  /* Clear I2S1 buffer to silence */
  memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
}

/**
 * @brief  Controls AUDIO Volume.
 * @param  vol: volume level (0..100)
 */
void Audio_Bridge_SetVolume(uint8_t vol)
{
/*
 * vol = 0..100 mapped to CS4304 atten, plus a fixed pad so 100% matches the
 * M031N amp input (~80 mV full drive from ~1.06 Vrms full scale ≈ −22 dB).
 * Set AMP_MATCH_PAD_HALFDB to 0 for full line-level output.
 */
#define AMP_MATCH_PAD_HALFDB 0u /* pad moved to external resistor divider \
                                 * (10k + 1k at amp input) for better SNR */
  uint32_t a = AMP_MATCH_PAD_HALFDB + ((vol >= 100) ? 0u : (100u - vol));
  uint8_t atten = (a > 255u) ? 255u : (uint8_t)a;
  if (s_dac != NULL)
  {
    CS4304_SetVolume(s_dac, atten);
  }
}

/**
 * @brief  Controls AUDIO Mute.
 * @param  cmd: command opcode
 */
void Audio_Bridge_SetMute(uint8_t cmd)
{
  if (s_dac != NULL)
  {
    CS4304_SetMute(s_dac, cmd);
  }
}

/**
 * @brief  Manages the DMA full transfer complete event.
 * @retval None
 */
void TransferComplete_CallBack_HS(void)
{
  dma_active_half = 0; /* DMA is now playing the first half, so second half is free to write */
}

/**
 * @brief  Manages the DMA Half transfer complete event.
 * @retval None
 */
void HalfTransfer_CallBack_HS(void)
{
  dma_active_half = 1; /* DMA is now playing the second half, so first half is free to write */
}

/**
 * @brief  Fill buffer with a 1 kHz sine test tone (32-bit, stereo).
 *         Used for testing all 4 DAC channels.
 * @param  buf         Pointer to stereo buffer (interleaved L+R, 32-bit)
 * @param  num_frames  Number of stereo frames to generate
 */
static void Audio_FillTestTone(int32_t *buf, uint32_t num_frames,
                               uint8_t chL, uint8_t chR)
{
  for (uint32_t i = 0; i < num_frames; i++)
  {
    buf[i * 2] = Audio_ToneDc_NextSample(chL);
    buf[i * 2 + 1] = Audio_ToneDc_NextSample(chR);
  }
}

/**
 * @brief  Fill ONE slot (0 = L, 1 = R) of an interleaved stereo buffer with
 *         a sine tone, leaving the other slot untouched. Used to put the
 *         CH2 tone in I2S1's right slot while USB audio owns the left slot.
 */
static void Audio_FillToneSlot(int32_t *buf, uint32_t num_frames,
                               uint8_t ch, uint8_t slot)
{
  for (uint32_t i = 0; i < num_frames; i++)
  {
    buf[i * 2 + slot] = Audio_ToneDc_NextSample(ch);
  }
}

/**
 * @brief  Fill ONE slot with the mixed N0–NF note bank. Only called while
 *         NoteBank_AnyActive() during the I2S DMA refill.
 *
 *         In AUDIO_CPULOAD_DMA mode, LED_Y is driven low for the duration of
 *         the fill (busy) and high afterward (idle) for scope duty-cycle.
 *         In AUDIO_CPULOAD_QUEUE mode, samples are pulled from the soft queue
 *         filled by Audio_CpuLoad_Poll() instead of calling NoteBank here.
 */
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames,
                                      uint8_t slot)
{
  if (Audio_CpuLoad_GetMode() == AUDIO_CPULOAD_QUEUE)
  {
    for (uint32_t i = 0; i < num_frames; i++)
    {
      buf[i * 2 + slot] = Audio_CpuLoad_QueuePop();
    }
    return;
  }

  if (Audio_CpuLoad_GetMode() == AUDIO_CPULOAD_DMA)
  {
    Audio_CpuLoad_LedBusy(1);
  }

  for (uint32_t i = 0; i < num_frames; i++)
  {
    buf[i * 2 + slot] = NoteBank_NextSample();
  }

  if (Audio_CpuLoad_GetMode() == AUDIO_CPULOAD_DMA)
  {
    Audio_CpuLoad_LedBusy(0);
  }
}

/** CH1 left: always SAMPLE note-bank mix (BODY fills rings, not I2S). */
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames)
{
  (void)Audio_Ch1NoteBankActive;
  Audio_FillCh1NoteBankSlot(buf, num_frames, 0);
}

Audio_CH1_Source_t Audio_GetCh1Source(void)
{
  return AUDIO_CH1_SRC_TEST_TONE;
}

/**
 * @brief  Start standalone I2S playback without waiting for USB.
 */
void Audio_StartPlayback(void)
{
  Audio_ToneDc_ResetPhases();

  if (!i2s_started)
  {
    memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
    memset(i2s2_tx_buf, 0, sizeof(i2s2_tx_buf));
    HAL_I2S_Transmit_DMA(&hi2s1, (uint16_t *)i2s1_tx_buf, AUDIO_I2S_BUF_SIZE);
#if AUDIO_USE_I2S2
    HAL_Delay(2);
    I2S2_Start();
#endif
    i2s_started = 1;
  }
}

/* --- I2S2 FIFO pump ---------------------------------------------------
 * The ONLY proven-working feed for SPI2-as-slave is polling-style TXDR
 * writes (HAL IT/DMA feeds freeze without error). TIM7 fires at 40 kHz and
 * tops up the FIFO exactly like the polling loop, generating the CH3/CH4
 * tones inline. HAL is armed once (SPE+CSTART) then kept dormant. */
volatile uint32_t g_pump_words = 0;      /* debugger: must keep rising */
volatile uint32_t g_i2s2_udr_clears = 0; /* UDR wedge recoveries */
static volatile uint8_t i2s2_pump_on = 0;
#if AUDIO_I2S2_IT
static uint8_t pump_slot = 0; /* 0 = CH3 (L), 1 = CH4 (R) */
#endif

void Audio_I2S2_Pump(void)
{
  if (!i2s2_pump_on)
  {
    return;
  }
  /* A slave-TX underrun (UDR) wedges the transmitter — it stops consuming
   * the FIFO until the flag is cleared (observed: SR=0x20, FIFO full, no
   * draining). Clear it every tick so transmission always resumes. */
  if ((SPI2->SR & SPI_SR_UDR) != 0u)
  {
    SPI2->IFCR = SPI_IFCR_UDRC;
    g_i2s2_udr_clears++;
  }
  if ((SPI2->SR & SPI_SR_TIFRE) != 0u)
  {
    SPI2->IFCR = SPI_IFCR_TIFREC;
  }

#if AUDIO_I2S2_IT
  /* CH2 tone/DC generation (left slot unused here since I2S2 is CH3/4)
   * The pump fills directly if in IT mode. */
  if (i2s2_pump_on && ((SPI2->SR & SPI_SR_TXP) != 0u))
  {
    int32_t s;
    if (pump_slot == 0)
    {
      s = Audio_ToneDc_NextSample(2); /* CH3 on I2S2 left slot */
    }
    else
    {
      s = Audio_ToneDc_NextSample(3); /* CH4 on I2S2 right slot */
    }
    SPI2->TXDR = (uint32_t)s;
    pump_slot ^= 1u;
    g_pump_words++;
  }
#endif
  /* DMA mode: feeding is done by DMA + half/complete callbacks; this tick
   * is only the UDR guard above. */
}

static void I2S2_PumpTimerInit(void)
{
  static uint8_t inited = 0;
  if (inited)
  {
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

/** Start the I2S2 transfer — interrupt mode (SPI2 DMA request line dead)
 * or DMA mode, per AUDIO_I2S2_IT. Pre-fills the tone so the first pass
 * is not silence. */
static HAL_StatusTypeDef I2S2_Start(void)
{
  Audio_FillTestTone(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
#if AUDIO_I2S2_IT
  {
    /* H7 SPI: a SLAVE transmits on MISO, but the board wires PC1 = MOSI to
     * the DAC's SDIN2 (the master-mode SDO pin). IOSWP swaps MISO/MOSI
     * inside the peripheral so the slave's data comes out on PC1.
     * CFG2 is only writable while SPE = 0. */
    CLEAR_BIT(SPI2->CR1, SPI_CR1_SPE);
    SET_BIT(SPI2->CFG2, SPI_CFG2_IOSWP);

    HAL_StatusTypeDef st = HAL_I2S_Transmit_IT(&hi2s2, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);
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

    HAL_StatusTypeDef st = HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t *)i2s2_tx_buf, AUDIO_I2S_BUF_SIZE);

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

/**
 * Fill one free half of i2s1_tx_buf (CH2 right + CH1 left).
 * Called from the I2S1 DMA half/full ISR.
 */
static void Audio_I2S1_FillHalf(uint8_t half)
{
  int32_t *buf =
      (half == 0u) ? &i2s1_tx_buf[0] : &i2s1_tx_buf[AUDIO_I2S_BUF_FRAMES];
  const uint32_t frames = AUDIO_I2S_BUF_FRAMES / 2u;

  if (i2s1_fill_busy != 0u)
  {
    g_i2s1_fill_late++;
  }
  i2s1_fill_busy = 1u;
  NoteBank_VmBoundaryBegin();
  Audio_FillToneSlot(buf, frames, 1, 1);
  Audio_RefillCh1Slot(buf, frames);
  NoteBank_VmBoundaryEnd();
  i2s1_fill_busy = 0u;
}

/**
 * @brief  Compatibility hook. I2S1 refill runs in the DMA callbacks.
 */
void Audio_I2S1_Poll(void)
{
}

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

/**
 * @brief  I2S1 DMA half transfer complete callback.
 *         DMA now plays the second half → refill the first.
 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s->Instance == SPI1)
  {
    HalfTransfer_CallBack_HS();
    Audio_I2S1_FillHalf(0u);
  }
  else if (hi2s->Instance == SPI2)
  {
    Audio_FillTestTone(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
  }
}

/**
 * @brief  I2S DMA full transfer complete callback.
 *         SPI1: DMA wrapped → refill the second half.
 */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s->Instance == SPI1)
  {
    TransferComplete_CallBack_HS();
    Audio_I2S1_FillHalf(1u);
  }
  else if (hi2s->Instance == SPI2)
  {
#if AUDIO_I2S2_IT
    /* IT mode: no half-complete events — refill the WHOLE buffer here,
     * then re-arm the interrupt transfer (self-sustaining chain). */
    Audio_FillTestTone(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES, 2, 3);
#else
    Audio_FillTestTone(&i2s2_tx_buf[AUDIO_I2S_BUF_FRAMES], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
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
  if (hi2s->Instance == SPI2)
  {
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
