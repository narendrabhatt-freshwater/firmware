/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : usbd_audio_if.c
 * @version        : v2.0_Channel_Card
 * @brief          : USB Audio to I2S bridge for CS4304 4-channel DAC.
 *
 *                   USB Audio (mono 32-bit 96kHz) → I2S1 (Ch1+Ch2)
 *                   I2S2 (Ch3+Ch4) available for testing.
 *
 *                   Data flow:
 *                     1. USB host sends mono 32-bit PCM samples
 *                     2. Audio_Bridge_WriteUSB() receives one USB packet
 *                     3. DMA half/full transfer callbacks trigger
 *                     4. Audio samples are duplicated from mono to stereo (L+R)
 *                        and placed into I2S1 DMA buffer
 *                     5. I2S2 outputs silence (or test tone when enabled)
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "audio_bridge.h"

/* USER CODE BEGIN INCLUDE */
#include "main.h"
#include "i2s.h"
#include "cs4304.h"
#include "audio_rate.h"
#include "note_bank.h"
#include <string.h>
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/*
 * I2S DMA buffer sizing:
 * I2S is configured for 32-bit data, stereo (L+R), at AUDIO_SAMPLE_RATE_HZ.
 * Each I2S frame = 2 × 32-bit words (L + R) = 8 bytes
 * DMA buffer is double-buffered: half/full IRQs each refill one half.
 *
 * AUDIO_I2S_BUF_FRAMES: stereo frames in the full DMA ring.
 * 192 frames = 2 ms full ring; half = 96 frames = 1 ms @ 96 kHz.
 * (Larger sizes were used for USB↔I2S SOF drift headroom — restore on a
 * dedicated USB branch if needed.)
 */
#define AUDIO_I2S_BUF_FRAMES 192                      /* 2 ms full; half = 96 = 1 ms @ 96 kHz */
#define AUDIO_I2S_BUF_SIZE (AUDIO_I2S_BUF_FRAMES * 2) /* × 2 for L+R, in 32-bit words */

/* I2S2 (CH3/CH4) enabled. Slave TX on SPI2 requires two workarounds
 * (full story in BRINGUP_REPORT.md):
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
/* I2S1 DMA buffer for channels 1+2 (USB audio output) */
static int32_t i2s1_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

/* I2S2 DMA buffer for channels 3+4 (test/other functions) */
static int32_t i2s2_tx_buf[AUDIO_I2S_BUF_SIZE] __attribute__((aligned(4), section(".dma_buffer")));

/* Audio playback state */
static volatile uint8_t audio_playing = 0;
static volatile uint8_t i2s_started = 0;

/* USB→I2S ring-buffer write index (32-bit words into i2s1_tx_buf).
 * Independent of the DMA half boundaries: initialised half a buffer ahead
 * of the DMA read point and re-synced only if clock drift erodes the lead.
 * (Writing per-half caused continuous glitching when USB SOF and I2S DMA
 * phase drifted across a half boundary.) */
static uint32_t usb_wr_idx = 0;
static volatile uint8_t usb_synced = 0;

/* Host mute state.  Volume itself is handled by Windows in software (the
 * device advertises a mute-only feature unit — see usb_descriptors.c), so
 * the USB stream only needs to gate to silence on mute. */
static volatile uint8_t usb_muted = 0;

void Audio_SetUSBMute(uint8_t mute) { usb_muted = mute ? 1u : 0u; }

/*
 * CPU load probe on LED_Y (PB9):
 *   low  = busy generating NoteBank samples
 *   high = idle (waiting for DMA / queue drain)
 * Scope duty: CPU% ≈ t_busy / (t_busy + t_idle).
 *
 * Queue mode: soft ring drained by DMA half/full callbacks (96 frames).
 * Capacity is larger than one half so the main-loop producer can stay ahead.
 * LED is idle only while the queue is full; otherwise Poll fills to full
 * (same duty-cycle idea as a ~16-sample low watermark with sample-at-a-time
 * drain — DMA gulps a half at once, so "not full → fill" is the safe rule).
 */
#define CPULOAD_Q_SIZE 256u
/* Classic sample-at-a-time low watermark is ~16; with DMA half gulps of
 * AUDIO_I2S_BUF_FRAMES/2 (96) we refill whenever the queue is not full instead. */

static volatile Audio_CpuLoadMode_t cpuload_mode = AUDIO_CPULOAD_OFF;

/* SPSC soft queue for AUDIO_CPULOAD_QUEUE (main produces, DMA consumes). */
static int32_t cpuload_q[CPULOAD_Q_SIZE];
static volatile uint32_t cpuload_q_wr = 0;
static volatile uint32_t cpuload_q_rd = 0;

static inline void CpuLoad_LedBusy(uint8_t busy)
{
  HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin,
                    busy ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static uint32_t CpuLoad_QueueCount(void)
{
  uint32_t wr = cpuload_q_wr;
  uint32_t rd = cpuload_q_rd;
  if (wr >= rd)
  {
    return wr - rd;
  }
  return CPULOAD_Q_SIZE - rd + wr;
}

static void CpuLoad_QueueReset(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  cpuload_q_wr = 0;
  cpuload_q_rd = 0;
  if (!primask)
  {
    __enable_irq();
  }
}

static void CpuLoad_QueuePush(int32_t s)
{
  uint32_t next = (cpuload_q_wr + 1u) % CPULOAD_Q_SIZE;
  if (next == cpuload_q_rd)
  {
    return; /* full — should not happen if producer respects FULL-1 */
  }
  cpuload_q[cpuload_q_wr] = s;
  cpuload_q_wr = next;
}

static int32_t CpuLoad_QueuePop(void)
{
  if (cpuload_q_wr == cpuload_q_rd)
  {
    return 0;
  }
  int32_t s = cpuload_q[cpuload_q_rd];
  cpuload_q_rd = (cpuload_q_rd + 1u) % CPULOAD_Q_SIZE;
  return s;
}

static void CpuLoad_QueuePrefill(void)
{
  while (CpuLoad_QueueCount() < (CPULOAD_Q_SIZE - 1u))
  {
    if (NoteBank_AnyActive())
    {
      CpuLoad_QueuePush(NoteBank_NextSample());
    }
    else
    {
      CpuLoad_QueuePush(0);
    }
  }
}

void Audio_CpuLoad_SetMode(Audio_CpuLoadMode_t mode)
{
  if (mode > AUDIO_CPULOAD_QUEUE)
  {
    mode = AUDIO_CPULOAD_OFF;
  }
  CpuLoad_QueueReset();
  cpuload_mode = mode;
  if (mode == AUDIO_CPULOAD_OFF)
  {
    HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin, GPIO_PIN_RESET);
  }
  else
  {
    if (mode == AUDIO_CPULOAD_QUEUE)
    {
      CpuLoad_QueuePrefill();
    }
    CpuLoad_LedBusy(0); /* idle high until next fill burst */
  }
}

Audio_CpuLoadMode_t Audio_CpuLoad_GetMode(void) { return cpuload_mode; }

uint8_t Audio_CpuLoad_IsActive(void)
{
  return (cpuload_mode != AUDIO_CPULOAD_OFF) ? 1u : 0u;
}

void Audio_CpuLoad_Poll(void)
{
  uint32_t count;

  if (cpuload_mode != AUDIO_CPULOAD_QUEUE)
  {
    return;
  }

  count = CpuLoad_QueueCount();
  /* Full → idle (LED high). Refill whenever not full so a 48-frame DMA
   * pop cannot underrun before the next main-loop Poll. */
  if (count >= (CPULOAD_Q_SIZE - 1u))
  {
    CpuLoad_LedBusy(0);
    return;
  }

  CpuLoad_LedBusy(1);
  while (CpuLoad_QueueCount() < (CPULOAD_Q_SIZE - 1u))
  {
    if (!NoteBank_AnyActive())
    {
      CpuLoad_QueuePush(0);
    }
    else
    {
      CpuLoad_QueuePush(NoteBank_NextSample());
    }
  }
  CpuLoad_LedBusy(0);
}

/* Test tone generator — independent frequency per channel (CH1..CH4).
 * All channels share fs = AUDIO_SAMPLE_RATE_HZ (single CS4304 clock domain). */
#define TEST_TONE_SAMPLE_RATE AUDIO_SAMPLE_RATE_HZ
/* CH1 = USB audio (no tone); CH2 = 1 kHz, CH3 = 2 kHz, CH4 = 3 kHz */
static uint32_t test_tone_freq_hz[4] = {1000, 1000, 2000, 3000}; /* CH1..CH4 (CH1 entry unused) */
static uint32_t test_tone_phase[4] = {0, 0, 0, 0};
/* Phase increment per sample (fixed-point 32-bit, full scale = 2^32),
 * precomputed per channel so ISRs never do 64-bit divides. */
#define TONE_PHASE_INC(f) ((uint32_t)(((uint64_t)(f) << 32) / TEST_TONE_SAMPLE_RATE))
static uint32_t test_tone_inc[4] = {0, 0, 0, 0};

/* --- Per-channel output mode: TONE (sine) or DC (constant level) ---
 * CH1 is always USB audio; modes only apply to CH2..CH4 generation. */
static Audio_ChannelMode_t channel_mode[4] = {AUDIO_MODE_TONE, AUDIO_MODE_TONE,
                                              AUDIO_MODE_TONE, AUDIO_MODE_TONE};
static int8_t dc_level_pct[4] = {0, 0, 0, 0};  /* -100..+100 percent (signed!) */
static int32_t dc_level_tgt[4] = {0, 0, 0, 0}; /* target Q31 sample value */
static int32_t dc_level_now[4] = {0, 0, 0, 0}; /* slewed current value */
/* Slew rate: full range (±0.5 FS) in ~21 ms at 96 kHz. An instant step
 * rings the DAC's interpolation filter → spikes on the control line. */
#define DC_SLEW_STEP (1L << 19)

/* Simple sine lookup table (256 entries, Q31 format) */
#define SINE_TABLE_SIZE 256
static const int32_t sine_table[SINE_TABLE_SIZE] = {
    0x00000000,
    0x0323ECBE,
    0x0647D97C,
    0x096A9049,
    0x0C8BD35E,
    0x0FAB272B,
    0x12C8106F,
    0x15E21445,
    0x18F8B83C,
    0x1C0B826A,
    0x1F19F97B,
    0x2223A4C5,
    0x25280C5E,
    0x2826B928,
    0x2B1F34EB,
    0x2E110A62,
    0x30FBC54D,
    0x33DEF287,
    0x36BA2014,
    0x398CDD32,
    0x3C56BA70,
    0x3F1749B8,
    0x41CE1E65,
    0x447ACD50,
    0x471CECE7,
    0x49B41533,
    0x4C3FDFF4,
    0x4EBFE8A5,
    0x5133CC94,
    0x539B2AF0,
    0x55F5A4D2,
    0x5842DD54,
    0x5A82799A,
    0x5CB420E0,
    0x5ED77C8A,
    0x60EC3830,
    0x62F201AC,
    0x64E88926,
    0x66CF8120,
    0x68A69E81,
    0x6A6D98A4,
    0x6C242960,
    0x6DCA0D14,
    0x6F5F02B2,
    0x70E2CBC6,
    0x72552C85,
    0x73B5EBD1,
    0x7504D345,
    0x7641AF3D,
    0x776C4EDB,
    0x78848414,
    0x798A23B1,
    0x7A7D055B,
    0x7B5D039E,
    0x7C29FBEE,
    0x7CE3CEB2,
    0x7D8A5F40,
    0x7E1D93EA,
    0x7E9D55FC,
    0x7F0991C4,
    0x7F62368F,
    0x7FA736B4,
    0x7FD8878E,
    0x7FF62182,
    0x7FFFFFFF,
    0x7FF62182,
    0x7FD8878E,
    0x7FA736B4,
    0x7F62368F,
    0x7F0991C4,
    0x7E9D55FC,
    0x7E1D93EA,
    0x7D8A5F40,
    0x7CE3CEB2,
    0x7C29FBEE,
    0x7B5D039E,
    0x7A7D055B,
    0x798A23B1,
    0x78848414,
    0x776C4EDB,
    0x7641AF3D,
    0x7504D345,
    0x73B5EBD1,
    0x72552C85,
    0x70E2CBC6,
    0x6F5F02B2,
    0x6DCA0D14,
    0x6C242960,
    0x6A6D98A4,
    0x68A69E81,
    0x66CF8120,
    0x64E88926,
    0x62F201AC,
    0x60EC3830,
    0x5ED77C8A,
    0x5CB420E0,
    0x5A82799A,
    0x5842DD54,
    0x55F5A4D2,
    0x539B2AF0,
    0x5133CC94,
    0x4EBFE8A5,
    0x4C3FDFF4,
    0x49B41533,
    0x471CECE7,
    0x447ACD50,
    0x41CE1E65,
    0x3F1749B8,
    0x3C56BA70,
    0x398CDD32,
    0x36BA2014,
    0x33DEF287,
    0x30FBC54D,
    0x2E110A62,
    0x2B1F34EB,
    0x2826B928,
    0x25280C5E,
    0x2223A4C5,
    0x1F19F97B,
    0x1C0B826A,
    0x18F8B83C,
    0x15E21445,
    0x12C8106F,
    0x0FAB272B,
    0x0C8BD35E,
    0x096A9049,
    0x0647D97C,
    0x0323ECBE,
    0x00000000,
    0xFCDC1342,
    0xF9B82684,
    0xF6956FB7,
    0xF3742CA2,
    0xF054D8D5,
    0xED37EF91,
    0xEA1DEBBB,
    0xE70747C4,
    0xE3F47D96,
    0xE0E60685,
    0xDDDC5B3B,
    0xDAD7F3A2,
    0xD7D946D8,
    0xD4E0CB15,
    0xD1EEF59E,
    0xCF043AB3,
    0xCC210D79,
    0xC945DFEC,
    0xC67322CE,
    0xC3A94590,
    0xC0E8B648,
    0xBE31E19B,
    0xBB8532B0,
    0xB8E31319,
    0xB64BEACD,
    0xB3C0200C,
    0xB140175B,
    0xAECC336C,
    0xAC64D510,
    0xAA0A5B2E,
    0xA7BD22AC,
    0xA57D8666,
    0xA34BDF20,
    0xA1288376,
    0x9F13C7D0,
    0x9D0DFE54,
    0x9B1776DA,
    0x99307EE0,
    0x9759617F,
    0x9592675C,
    0x93DBD6A0,
    0x9235F2EC,
    0x90A0FD4E,
    0x8F1D343A,
    0x8DAAD37B,
    0x8C4A142F,
    0x8AFB2CBB,
    0x89BE50C3,
    0x8893B125,
    0x877B7BEC,
    0x8675DC4F,
    0x8782FAA5,
    0x84A2FC62,
    0x83D60412,
    0x831C314E,
    0x8275A0C0,
    0x81E26C16,
    0x8162AA04,
    0x80F66E3C,
    0x809DC971,
    0x8058C94C,
    0x80277872,
    0x8009DE7E,
    0x80000001,
    0x8009DE7E,
    0x80277872,
    0x8058C94C,
    0x809DC971,
    0x80F66E3C,
    0x8162AA04,
    0x81E26C16,
    0x8275A0C0,
    0x831C314E,
    0x83D60412,
    0x84A2FC62,
    0x8782FAA5,
    0x8675DC4F,
    0x877B7BEC,
    0x8893B125,
    0x89BE50C3,
    0x8AFB2CBB,
    0x8C4A142F,
    0x8DAAD37B,
    0x8F1D343A,
    0x90A0FD4E,
    0x9235F2EC,
    0x93DBD6A0,
    0x9592675C,
    0x9759617F,
    0x99307EE0,
    0x9B1776DA,
    0x9D0DFE54,
    0x9F13C7D0,
    0xA1288376,
    0xA34BDF20,
    0xA57D8666,
    0xA7BD22AC,
    0xAA0A5B2E,
    0xAC64D510,
    0xAECC336C,
    0xB140175B,
    0xB3C0200C,
    0xB64BEACD,
    0xB8E31319,
    0xBB8532B0,
    0xBE31E19B,
    0xC0E8B648,
    0xC3A94590,
    0xC67322CE,
    0xC945DFEC,
    0xCC210D79,
    0xCF043AB3,
    0xD1EEF59E,
    0xD4E0CB15,
    0xD7D946D8,
    0xDAD7F3A2,
    0xDDDC5B3B,
    0xE0E60685,
    0xE3F47D96,
    0xE70747C4,
    0xEA1DEBBB,
    0xED37EF91,
    0xF054D8D5,
    0xF3742CA2,
    0xF6956FB7,
    0xF9B82684,
    0xFCDC1342,
};

/** Next tone sample for one channel, with LINEAR INTERPOLATION between
 * sine-table entries. Plain 8-bit table lookup quantizes the phase to
 * 1/256 cycle, making zero-crossings jitter by up to ~4 µs at 1 kHz
 * ("shaky" waveform on the scope). Interpolation removes it. -6 dBFS.
 * Cost: ~10 cycles/sample — negligible at 550 MHz. */
static inline int32_t Tone_NextSineSample(uint8_t ch)
{
  uint32_t ph = test_tone_phase[ch];
  uint8_t idx = (uint8_t)(ph >> 24);
  int32_t s0 = sine_table[idx];
  int32_t s1 = sine_table[(uint8_t)(idx + 1u)];
  uint32_t frac = (ph >> 8) & 0xFFFFu; /* 16-bit fraction */
  int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)frac) >> 16);

  test_tone_phase[ch] = ph + test_tone_inc[ch];
  return s >> 1; /* -6 dB headroom */
}

/** CH1 left slot plays the N0–NF note bank when any voice is active. */
static inline uint8_t Audio_Ch1NoteBankActive(void)
{
  return NoteBank_AnyActive();
}

/** Next sample for one channel — returns sine tone or DC level depending
 * on the channel mode. CH1 (index 0) is always USB audio so this is
 * only meaningful for CH2..CH4 (indices 1..3). */
static inline int32_t Audio_NextSample(uint8_t ch)
{
  if (channel_mode[ch] == AUDIO_MODE_DC)
  {
    /* Slew-limited approach to the target: no steps, no filter ringing */
    int32_t now = dc_level_now[ch];
    int32_t tgt = dc_level_tgt[ch];
    if (now < tgt)
    {
      now += DC_SLEW_STEP;
      if (now > tgt)
      {
        now = tgt;
      }
    }
    else if (now > tgt)
    {
      now -= DC_SLEW_STEP;
      if (now < tgt)
      {
        now = tgt;
      }
    }
    dc_level_now[ch] = now;
    return now;
  }
  return Tone_NextSineSample(ch);
}

/* CS4304 handle (defined in main.c, extern here) */
extern CS4304_HandleTypeDef hcs4304;

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
 * @brief Usb device library.
 * @{
 */

/** @addtogroup USBD_AUDIO_IF
 * @{
 */

/** @defgroup USBD_AUDIO_IF_Private_TypesDefinitions USBD_AUDIO_IF_Private_TypesDefinitions
 * @brief Private types.
 * @{
 */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
 * @}
 */

/** @defgroup USBD_AUDIO_IF_Private_Defines USBD_AUDIO_IF_Private_Defines
 * @brief Private defines.
 * @{
 */

/* USER CODE BEGIN PRIVATE_DEFINES */

/* USER CODE END PRIVATE_DEFINES */

/**
 * @}
 */

/** @defgroup USBD_AUDIO_IF_Private_Macros USBD_AUDIO_IF_Private_Macros
 * @brief Private macros.
 * @{
 */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
 * @}
 */

/** @defgroup USBD_AUDIO_IF_Private_Variables USBD_AUDIO_IF_Private_Variables
 * @brief Private variables.
 * @{
 */

/* USER CODE BEGIN PRIVATE_VARIABLES */
static volatile uint8_t dma_active_half = 0; /* 0 = playing first half (write to second), 1 = playing second half (write to first) */

/* I2S1 sample fill runs in main (Audio_I2S1_Poll), not in the DMA IRQ.
 * ISR only updates USB half bookkeeping and posts which half is free. */
static volatile uint8_t i2s1_fill_pending = 0;
static volatile uint8_t i2s1_fill_half = 0; /* 0 = first half, 1 = second half */
volatile uint32_t g_i2s1_fill_late = 0;     /* ISR saw pending still set (debugger) */
/* USER CODE END PRIVATE_VARIABLES */

/**
 * @}
 */

/** @defgroup USBD_AUDIO_IF_Exported_Variables USBD_AUDIO_IF_Exported_Variables
 * @brief Public variables.
 * @{
 */

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
 * @}
 */

/** @defgroup USBD_AUDIO_IF_Private_FunctionPrototypes USBD_AUDIO_IF_Private_FunctionPrototypes
 * @brief Private functions declaration.
 * @{
 */

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */
static void Audio_FillTestTone(int32_t *buf, uint32_t num_frames,
                               uint8_t chL, uint8_t chR);
static void Audio_FillToneSlot(int32_t *buf, uint32_t num_frames,
                               uint8_t ch, uint8_t slot);
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames,
                                      uint8_t slot);
static void Audio_FillCh1SilenceSlot(int32_t *buf, uint32_t num_frames,
                                     uint8_t slot);
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames);
static HAL_StatusTypeDef I2S2_Start(void);
/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
 * @}
 */

/* Private functions ---------------------------------------------------------*/
/**
 * @brief  Initializes the AUDIO media low layer over the USB HS IP
 * @param  AudioFreq: Audio frequency used to play the audio stream.
 * @param  Volume: Initial volume level (from 0 (Mute) to 100 (Max))
 * @param  options: Reserved for future use
 */
void Audio_Bridge_Start(void)
{
  /* USER CODE BEGIN 9 */
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
    /* Let BCLK/WS run before enabling the slave. MUST be a busy-wait:
     * Audio_Bridge_Start runs in USB interrupt context where HAL_Delay
     * deadlocks (SysTick cannot preempt the USB IRQ). ~2 ms at 550 MHz. */
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
   * The ST UAC1 class exposes only MUTE to the host, so the OS applies
   * volume in software by scaling the PCM samples — no hardware volume
   * write is needed here (and overriding the bring-up attenuation with
   * the class default would be wrong).
   */
  /* USER CODE END 9 */
}

/**
 * @brief  DeInitializes the AUDIO media low layer
 * @param  options: Reserved for future use
 */
void Audio_Bridge_Stop(void)
{
  /* USER CODE BEGIN 10 */
  /* Stop I2S DMA */
  HAL_I2S_DMAStop(&hi2s1);
#if AUDIO_USE_I2S2
  HAL_I2S_DMAStop(&hi2s2);
#endif
  i2s_started = 0;
  audio_playing = 0;
  usb_synced = 0;
  /* USER CODE END 10 */
}

/**
 * @brief  Handles AUDIO command.
 * @param  pbuf: Pointer to buffer of data to be sent
 * @param  size: Number of data to be sent (in bytes)
 * @param  cmd: Command opcode
 */
void Audio_Bridge_StreamStop(void)
{
  /* USER CODE BEGIN 11 */
  audio_playing = 0;
  usb_synced = 0; /* re-sync write lead on next stream */
  /* Clear I2S1 buffer to silence */
  memset(i2s1_tx_buf, 0, sizeof(i2s1_tx_buf));
  /* USER CODE END 11 */
}

/**
 * @brief  Controls AUDIO Volume.
 * @param  vol: volume level (0..100)
 */
void Audio_Bridge_SetVolume(uint8_t vol)
{
/* USER CODE BEGIN 12 */
/*
 * vol = 0..100, linear in dB across the advertised range (vol_min..vol_max
 * = −50..0 dB, see USBD_AUDIO_Init), plus a fixed pad so 100% matches the
 * M031N amp input (~80 mV full drive from ~1.06 Vrms full scale ≈ −22 dB).
 * Set AMP_MATCH_PAD_HALFDB to 0 for full line-level output.
 */
#define AMP_MATCH_PAD_HALFDB 0u /* pad moved to external resistor divider \
                                 * (10k + 1k at amp input) for better SNR */
  uint32_t a = AMP_MATCH_PAD_HALFDB + ((vol >= 100) ? 0u : (100u - vol));
  uint8_t atten = (a > 255u) ? 255u : (uint8_t)a;
  CS4304_SetVolume(&hcs4304, atten);
  /* USER CODE END 12 */
}

/**
 * @brief  Controls AUDIO Mute.
 * @param  cmd: command opcode
 */
void Audio_Bridge_SetMute(uint8_t cmd)
{
  /* USER CODE BEGIN 13 */
  CS4304_SetMute(&hcs4304, cmd);
  /* USER CODE END 13 */
}

/**
 * @brief  Audio_Bridge_WriteUSB
 * @param  cmd: command opcode
 */
void Audio_Bridge_WriteUSB(const uint8_t *pbuf, uint32_t size)
{
  /* USER CODE BEGIN 14 */
  /* Called once per USB isochronous OUT packet (1 ms of audio).  Under
   * TinyUSB this is driven directly by the audio class RX callback, so
   * there is no command opcode to test — a packet arriving *is* the
   * "host is streaming" signal. */
  {
    /* Note bank owns the left slot while any N0–NF voice is active —
     * ignore USB packets instead of fighting the DMA refill. */
    if (Audio_Ch1NoteBankActive())
    {
      return;
    }

    audio_playing = 1;
    uint32_t num_mono_samples = size / 4; /* 4 bytes per 32-bit sample */

    /* Current DMA read position in words (NDTR counts remaining transfers) */
    DMA_Stream_TypeDef *st = (DMA_Stream_TypeDef *)hi2s1.hdmatx->Instance;
    uint32_t rd = (AUDIO_I2S_BUF_SIZE - st->NDTR) % AUDIO_I2S_BUF_SIZE;

    if (!usb_synced)
    {
      /* Start writing half a buffer (1 ms @ 96 kHz) ahead of the DMA read point */
      usb_wr_idx = ((rd + AUDIO_I2S_BUF_SIZE / 2u) % AUDIO_I2S_BUF_SIZE) & ~1u;
      usb_synced = 1;
    }
    else
    {
      /* Drift guard: if host/device clock drift erodes (or overgrows) the
       * lead, re-centre. Causes one tiny discontinuity every few minutes
       * instead of continuous glitching. */
      uint32_t lead = (usb_wr_idx + AUDIO_I2S_BUF_SIZE - rd) % AUDIO_I2S_BUF_SIZE;
      if (lead < AUDIO_I2S_BUF_SIZE / 4u || lead > (3u * AUDIO_I2S_BUF_SIZE) / 4u)
      {
        usb_wr_idx = ((rd + AUDIO_I2S_BUF_SIZE / 2u) % AUDIO_I2S_BUF_SIZE) & ~1u;
      }
    }

    /* USB mono → LEFT slot only (CH1). Right slot (CH2) carries the tone,
     * written by the DMA callbacks — do not touch it here.
     * Volume is already applied by Windows in software; the device only
     * gates to silence on mute (exact passthrough otherwise). */
    const int32_t *src = (const int32_t *)pbuf;
    uint8_t muted = usb_muted;
    for (uint32_t i = 0; i < num_mono_samples; i++)
    {
      i2s1_tx_buf[usb_wr_idx] = muted ? 0 : src[i]; /* Left = CH1 = USB */
      usb_wr_idx = (usb_wr_idx + 2u) % AUDIO_I2S_BUF_SIZE;
    }
  }
  /* USER CODE END 14 */
}

/**
 * @brief  Manages the DMA full transfer complete event.
 * @retval None
 */
void TransferComplete_CallBack_HS(void)
{
  /* USER CODE BEGIN 16 */
  dma_active_half = 0; /* DMA is now playing the first half, so second half is free to write */
  /* (ST's USBD_AUDIO_Sync() bookkeeping is gone: TinyUSB's audio class
   * tracks its own FIFO, and the USB write pointer is positioned from the
   * DMA's NDTR directly in Audio_Bridge_WriteUSB().) */
  /* USER CODE END 16 */
}

/**
 * @brief  Manages the DMA Half transfer complete event.
 * @retval None
 */
void HalfTransfer_CallBack_HS(void)
{
  /* USER CODE BEGIN 17 */
  dma_active_half = 1; /* DMA is now playing the second half, so first half is free to write */
  /* USER CODE END 17 */
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

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
    buf[i * 2] = Audio_NextSample(chL);
    buf[i * 2 + 1] = Audio_NextSample(chR);
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
    buf[i * 2 + slot] = Audio_NextSample(ch);
  }
}

/**
 * @brief  Fill ONE slot with the mixed N0–NF note bank. Only called while
 *         NoteBank_AnyActive() (see Audio_I2S1_Poll) — otherwise CH1's
 *         slot is left for Audio_Bridge_WriteUSB() to fill from USB.
 *
 *         In AUDIO_CPULOAD_DMA mode, LED_Y is driven low for the duration of
 *         the fill (busy) and high afterward (idle) for scope duty-cycle.
 *         In AUDIO_CPULOAD_QUEUE mode, samples are pulled from the soft queue
 *         filled by Audio_CpuLoad_Poll() instead of calling NoteBank here.
 */
static void Audio_FillCh1NoteBankSlot(int32_t *buf, uint32_t num_frames,
                                      uint8_t slot)
{
  if (cpuload_mode == AUDIO_CPULOAD_QUEUE)
  {
    for (uint32_t i = 0; i < num_frames; i++)
    {
      buf[i * 2 + slot] = CpuLoad_QueuePop();
    }
    return;
  }

  if (cpuload_mode == AUDIO_CPULOAD_DMA)
  {
    CpuLoad_LedBusy(1);
  }

  for (uint32_t i = 0; i < num_frames; i++)
  {
    buf[i * 2 + slot] = NoteBank_NextSample();
  }

  if (cpuload_mode == AUDIO_CPULOAD_DMA)
  {
    CpuLoad_LedBusy(0);
  }
}

/**
 * Zero CH1's interleaved slot. Used when the note bank just went idle and
 * USB is not streaming — otherwise the DMA ring keeps replaying the last
 * half-buffer of sine forever (looks like "stuck" tone on a scope with
 * USB unplugged / RS485-only MIDI).
 */
static void Audio_FillCh1SilenceSlot(int32_t *buf, uint32_t num_frames,
                                     uint8_t slot)
{
  for (uint32_t i = 0; i < num_frames; i++)
  {
    buf[i * 2 + slot] = 0;
  }
}

/** CH1 left: note bank, else USB (untouched here), else silence. */
static void Audio_RefillCh1Slot(int32_t *buf, uint32_t num_frames)
{
  if (Audio_Ch1NoteBankActive() || cpuload_mode == AUDIO_CPULOAD_QUEUE)
  {
    Audio_FillCh1NoteBankSlot(buf, num_frames, 0);
  }
  else if (!audio_playing)
  {
    Audio_FillCh1SilenceSlot(buf, num_frames, 0);
  }
  /* else: USB owns the left slot via Audio_Bridge_WriteUSB(). */
}

/**
 * @brief  Set the test tone frequency for one channel (1..4), in Hz.
 *         Takes effect immediately while the tone is running.
 */
void Audio_SetToneFreq(uint8_t channel, uint32_t freq_hz)
{
  if (channel >= 1 && channel <= 4 && freq_hz > 0 && freq_hz < 20000)
  {
    test_tone_freq_hz[channel - 1] = freq_hz;
    test_tone_inc[channel - 1] = TONE_PHASE_INC(freq_hz);
  }
}

/**
 * @brief  Get the current test tone frequency for one channel (1..4).
 * @retval frequency in Hz, 0 if channel invalid
 */
uint32_t Audio_GetToneFreq(uint8_t channel)
{
  return (channel >= 1 && channel <= 4) ? test_tone_freq_hz[channel - 1] : 0;
}

Audio_CH1_Source_t Audio_GetCh1Source(void)
{
  return Audio_Ch1NoteBankActive() ? AUDIO_CH1_SRC_TEST_TONE
                                   : AUDIO_CH1_SRC_USB;
}

/**
 * @brief  Start standalone I2S playback without waiting for USB.
 */
void Audio_StartPlayback(void)
{
  test_tone_phase[0] = test_tone_phase[1] = 0;
  test_tone_phase[2] = test_tone_phase[3] = 0;
  for (uint8_t i = 0; i < 4; i++)
  {
    test_tone_inc[i] = TONE_PHASE_INC(test_tone_freq_hz[i]);
  }

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

/* --- DC level / channel mode API ----------------------------------------- */

void Audio_SetChannelMode(uint8_t channel, Audio_ChannelMode_t mode)
{
  if (channel >= 2 && channel <= 4)
  {
    channel_mode[channel - 1] = mode;
  }
}

Audio_ChannelMode_t Audio_GetChannelMode(uint8_t channel)
{
  return (channel >= 1 && channel <= 4) ? channel_mode[channel - 1] : AUDIO_MODE_TONE;
}

/* Symmetric DC range cap, in % of DAC full scale. The analog chain rides on
 * VMID, so headroom is asymmetric — the smaller (negative-going) side sets
 * the usable range and BOTH polarities are capped to it, keeping the output
 * symmetric about the VMID center. Positive headroom above the cap is
 * intentionally unused. Adjustable at runtime via 'dcmax' console command. */
static uint8_t dc_fs_limit_pct = 50;

/* Per-channel sign inversion (bit = channel index). The analog conditioning
 * stages are inverting, so this lets 'dc <ch> +N' mean a POSITIVE voltage at
 * the final control point regardless of the analog chain's sign. */
static uint8_t dc_invert_mask = 0;

/* Per-channel calibrated zero: the dc-code (in % units) at which the ANALOG
 * output crosses 0 V (the VMID offset seen through the conditioning stage).
 * With zero Z set, user percent p maps to:  eff = Z + p*(100-|Z|)/100
 *   dc 0    -> true 0 V at the output
 *   dc +100 -> full reach on the wide side (old +100)
 *   dc -100 -> the voltage MIRROR of +100 around the new zero
 * so the control is symmetric in physical volts about 0 V. */
/* Board calibration (measured 2026-07-13): output crosses 0 V at these
 * dc codes — CH2: +32, CH3: +22, CH4: +32. Adjustable at runtime via
 * 'dczero'; re-measure if VMID or the conditioning stages change. */
static int8_t dc_zero_pct[4] = {0, 32, 22, 32};

/* Fine zero trim in 0.01% units (±9.99%), added on top of dc_zero_pct.
 * Replaces an analog trimmer: with dcmax 50 one step ≈ 0.13 mV at the
 * output — nulls diff-amp resistor-tolerance residuals digitally. */
static int16_t dc_trim_x100[4] = {0, 0, 0, 0};

static int32_t DC_PctToTarget(uint8_t idx, int8_t percent)
{
  int32_t p = ((dc_invert_mask >> idx) & 1u) ? -percent : percent;
  int32_t z = dc_zero_pct[idx];
  int32_t az = (z < 0) ? -z : z;
  /* effective percent × 100 for precision: z*100 + p*(100-|z|)  (|..| ≤ 10000) */
  int32_t eff_x100 = z * 100 + p * (100 - az) + dc_trim_x100[idx];
  return (int32_t)(((int64_t)0x7FFFFFFF * eff_x100 * dc_fs_limit_pct) / 1000000);
}

void Audio_SetDCLevel(uint8_t channel, int8_t percent)
{
  if (channel >= 2 && channel <= 4 && percent >= -100 && percent <= 100)
  {
    uint8_t idx = channel - 1;
    dc_level_pct[idx] = percent;
    /* The generator slews toward this target (DC_SLEW_STEP) — no spikes. */
    dc_level_tgt[idx] = DC_PctToTarget(idx, percent);
    channel_mode[idx] = AUDIO_MODE_DC; /* auto-switch to DC mode */
  }
}

void Audio_SetDCLimit(uint8_t pct_fs)
{
  if (pct_fs >= 5 && pct_fs <= 100)
  {
    dc_fs_limit_pct = pct_fs;
    /* Re-scale all active DC channels to the new cap (slewed, no spikes) */
    for (uint8_t i = 1; i < 4; i++)
    {
      if (channel_mode[i] == AUDIO_MODE_DC)
      {
        dc_level_tgt[i] = DC_PctToTarget(i, dc_level_pct[i]);
      }
    }
  }
}

uint8_t Audio_GetDCLimit(void)
{
  return dc_fs_limit_pct;
}

void Audio_SetDCInvert(uint8_t channel, uint8_t invert)
{
  if (channel >= 2 && channel <= 4)
  {
    uint8_t idx = channel - 1;
    if (invert)
    {
      dc_invert_mask |= (uint8_t)(1u << idx);
    }
    else
    {
      dc_invert_mask &= (uint8_t)~(1u << idx);
    }
    if (channel_mode[idx] == AUDIO_MODE_DC)
    {
      dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]); /* re-apply */
    }
  }
}

uint8_t Audio_GetDCInvert(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? ((dc_invert_mask >> (channel - 1)) & 1u) : 0;
}

void Audio_SetDCZero(uint8_t channel, int8_t zero_pct)
{
  if (channel >= 2 && channel <= 4 && zero_pct >= -99 && zero_pct <= 99)
  {
    uint8_t idx = channel - 1;
    dc_zero_pct[idx] = zero_pct;
    /* Calibrating the zero implies DC output: switch mode and apply
     * immediately so the effect is always visible (like Audio_SetDCLevel). */
    channel_mode[idx] = AUDIO_MODE_DC;
    dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]);
  }
}

int8_t Audio_GetDCZero(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_zero_pct[channel - 1] : 0;
}

void Audio_SetDCTrim(uint8_t channel, int16_t trim_x100)
{
  if (channel >= 2 && channel <= 4 && trim_x100 >= -999 && trim_x100 <= 999)
  {
    uint8_t idx = channel - 1;
    dc_trim_x100[idx] = trim_x100;
    channel_mode[idx] = AUDIO_MODE_DC; /* trimming implies DC output */
    dc_level_tgt[idx] = DC_PctToTarget(idx, dc_level_pct[idx]);
  }
}

int16_t Audio_GetDCTrim(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_trim_x100[channel - 1] : 0;
}

int8_t Audio_GetDCLevel(uint8_t channel)
{
  return (channel >= 2 && channel <= 4) ? dc_level_pct[channel - 1] : 0;
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
      s = Audio_NextSample(2); /* CH3 on I2S2 left slot */
    }
    else
    {
      s = Audio_NextSample(3); /* CH4 on I2S2 right slot */
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
  HAL_NVIC_SetPriority(TIM7_IRQn, 0, 0); /* must not be starved: the FIFO
                                            is only ~20 µs deep at 96 kHz */
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
 * Fill one free half of i2s1_tx_buf (CH2 right + CH1 left). Called from
 * Audio_I2S1_Poll in the main loop — never from the DMA IRQ.
 */
static void Audio_I2S1_FillHalf(uint8_t half)
{
  int32_t *buf =
      (half == 0u) ? &i2s1_tx_buf[0] : &i2s1_tx_buf[AUDIO_I2S_BUF_FRAMES];
  const uint32_t frames = AUDIO_I2S_BUF_FRAMES / 2u;

  Audio_FillToneSlot(buf, frames, 1, 1);
  Audio_RefillCh1Slot(buf, frames);
}

/**
 * @brief  Service a pending I2S1 half-buffer refill. Call first in while(1).
 */
void Audio_I2S1_Poll(void)
{
  uint8_t half;
  uint32_t primask;

  if (i2s1_fill_pending == 0u)
  {
    return;
  }

  /* Claim half under IRQ mask so a DMA edge cannot overwrite mid-read. */
  primask = __get_PRIMASK();
  __disable_irq();
  if (i2s1_fill_pending == 0u)
  {
    if (!primask)
    {
      __enable_irq();
    }
    return;
  }
  half = i2s1_fill_half;
  i2s1_fill_pending = 0u;
  if (!primask)
  {
    __enable_irq();
  }

  Audio_I2S1_FillHalf(half);
}

/**
 * @brief  I2S1 DMA half transfer complete callback.
 *         Bookkeeping + post first-half fill to main; do not generate samples.
 */
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s->Instance == SPI1)
  {
    /*
     * DMA now plays the second half → first half is free for writing.
     * Must run every half-period for USB↔I2S handoff (dma_active_half).
     * Sample fill is deferred to Audio_I2S1_Poll() so NoteBank does not
     * starve the main loop from IRQ context.
     */
    HalfTransfer_CallBack_HS();

    if (i2s1_fill_pending != 0u)
    {
      g_i2s1_fill_late++;
    }
    i2s1_fill_half = 0u;
    i2s1_fill_pending = 1u;
  }
  else if (hi2s->Instance == SPI2)
  {
    Audio_FillTestTone(&i2s2_tx_buf[0], AUDIO_I2S_BUF_FRAMES / 2, 2, 3);
  }
}

/**
 * @brief  I2S DMA full transfer complete callback.
 *         SPI1: bookkeeping + post second-half fill to main.
 */
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
  if (hi2s->Instance == SPI1)
  {
    /* DMA wrapped: now playing the first half → second half is writable. */
    TransferComplete_CallBack_HS();

    if (i2s1_fill_pending != 0u)
    {
      g_i2s1_fill_late++;
    }
    i2s1_fill_half = 1u;
    i2s1_fill_pending = 1u;
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

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
 * @}
 */

/**
 * @}
 */
