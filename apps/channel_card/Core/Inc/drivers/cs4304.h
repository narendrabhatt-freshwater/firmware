/**
  ******************************************************************************
  * @file    cs4304.h
  * @brief   CS4304S 4-Channel DAC Driver (I2C software-control mode)
  *          Register map verified against Cirrus DS1388F1 (JUL 2025), §6/§7.
  *
  *          Control port (§4.9.1):
  *            - 16-bit register addresses, 16-bit data words, MSB first.
  *            - Only word transactions allowed; address LSB must be 0.
  *            - I2C address set by CONFIG5 (Table 4-17). 0Ω to GND -> 0x60.
  *
  *          Key behavior (§4.3, §4.5):
  *            - SW_RESET: write 0x5A to bits[15:8] of reg 0x0022.
  *            - Clocking/ASP registers latch ONLY on GLOBAL_EN rising edge.
  *            - All channels default MUTED (OUTx_MUTE = 1).
  *            - Volume/mute take effect only after writing 1 to OUT_VU.
  *            - Output active ~1 s after enable (STARTUP_DELAY default).
  *
 *          Board configuration (this project):
 *            PC7  -> DAC_RST, PA8 -> MCLK (MCO1 = 24.576 MHz = 512·fs @ 48 kHz)
 *            PC10 -> I2C5_SDA / PC11 -> I2C5_SCL
 *            I2S1 (Master TX) -> ASP_DIN1 (Ch1+Ch2)
 *            I2S2 (Slave  TX) -> ASP_DIN2 (Ch3+Ch4)
 *            fs = 48 kHz, BCLK = 64·fs = 3.072 MHz, I2S, ASP Secondary.
 *            Clocking: PLL enabled, MCLK reference @ 24.576 MHz.
  ******************************************************************************
  */

#ifndef __CS4304_H
#define __CS4304_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "i2c.h"

/* Exported types ------------------------------------------------------------*/

typedef enum {
  CS4304_STATE_RESET = 0,
  CS4304_STATE_READY,
  CS4304_STATE_PLAYING,
  CS4304_STATE_MUTED,
  CS4304_STATE_ERROR
} CS4304_StateTypeDef;

typedef struct {
  I2C_HandleTypeDef    *hi2c;     /**< I2C handle for control port */
  uint16_t              DevAddr;  /**< 8-bit (left-shifted) I2C address; 0 = scan bus */
  uint16_t              DevId;    /**< DEVID read at init (expect 0x43B4) */
  CS4304_StateTypeDef   State;
  uint8_t               Volume;   /**< Master attenuation, 0.5 dB steps: 0x00 = 0 dB, 0xFF = −127.5 dB */
  uint8_t               Trim[4];  /**< Per-channel EXTRA attenuation (0.5 dB steps), added to Volume.
                                       Survives master volume changes (USB slider). */
  uint8_t               MasterMask; /**< Bit i = 1: channel i+1 follows master Volume.
                                         Bit i = 0: channel i+1 ignores master (level = Trim only).
                                         Default 0x0F (all follow). */
  uint8_t               IsMuted;
} CS4304_HandleTypeDef;

/* I2C addresses (Table 4-17, write address = 8-bit HAL format) --------------*/
#define CS4304_I2C_ADDR_C5_GND_0R    0x60  /**< CONFIG5 0Ω pull-down to GND_A */
#define CS4304_I2C_ADDR_C5_GND_4K7   0x62
#define CS4304_I2C_ADDR_C5_GND_22K   0x64
#define CS4304_I2C_ADDR_C5_GND_100K  0x66
#define CS4304_I2C_ADDR_C5_VDD_100K  0x68
#define CS4304_I2C_ADDR_C5_VDD_22K   0x6A
#define CS4304_I2C_ADDR_C5_VDD_4K7   0x6C
#define CS4304_I2C_ADDR_C5_VDD_0R    0x6E  /**< CONFIG5 0Ω pull-up to VDD_A */

/* Register map (DS1388F1 §6, 16-bit addresses) -------------------------------*/
#define CS4304_REG_DEVID             0x0000  /* RO, = 0x43B4 */
#define CS4304_REG_REVID             0x0004  /* RO */
#define CS4304_REG_SW_RESET          0x0022  /* WO, write 0x5A00 to reset */
#define CS4304_REG_CLK_CFG_0         0x0040
#define CS4304_REG_CLK_CFG_1         0x0042
#define CS4304_REG_CHIP_ENABLE       0x0044
#define CS4304_REG_ASP_CFG           0x0048
#define CS4304_REG_SIGNAL_PATH_CFG   0x0050
#define CS4304_REG_OUT_ENABLES       0x00C0
#define CS4304_REG_OUT_RAMP_SUM      0x00C2
#define CS4304_REG_OUT_FILTER        0x00C6
#define CS4304_REG_OUT_INV           0x00CA
#define CS4304_REG_OUT_VOL_CTRL1_0   0x00D0  /* Ch1: bit15 = MUTE, bits[7:0] = VOL */
#define CS4304_REG_OUT_VOL_CTRL1_1   0x00D2  /* Ch2 */
#define CS4304_REG_OUT_VOL_CTRL2_0   0x00D4  /* Ch3 */
#define CS4304_REG_OUT_VOL_CTRL2_1   0x00D6  /* Ch4 */
#define CS4304_REG_OUT_VOL_CTRL5     0x00E0  /* WO, bit0 = OUT_VU latch */
#define CS4304_REG_SHUTDOWN_CTRL     0x00E4
#define CS4304_REG_STARTUP_DELAY     0x00E6

/* Field values ---------------------------------------------------------------*/
#define CS4304_DEVID_VALUE           0x43B4
#define CS4304_SW_RESET_KEY          0x5A00  /* SW_RESET is bits[15:8] */

/* CLK_CFG_0: SYSCLK_SRC=1 (PLL, bit12) | PLL_REFCLK_FREQ=11 (24.576 MHz,
 * bits[5:4]) | PLL_REFCLK_SRC=1 (MCLK, bit0)                                  */
#define CS4304_CLK_CFG_0_PLL_MCLK_24M576   0x1031
/* CLK_CFG_1: SAMPLE_RATE bits[2:0] (CS4304 / CS530x family):
 *   001 = 48/44.1 kHz, 010 = 96/88.2 kHz, 110 = autodetect                    */
#define CS4304_SAMPLE_RATE_48K       0x0001
#define CS4304_SAMPLE_RATE_96K       0x0002
#define CS4304_SAMPLE_RATE_AUTO      0x0006
/** Active bring-up rate — must match AUDIO_SAMPLE_RATE_HZ / I2S AudioFreq. */
#define CS4304_SAMPLE_RATE_ACTIVE    CS4304_SAMPLE_RATE_48K
/* ASP_CFG: secondary mode, BCLK non-inverted, BCLK_FREQ = 64 fs (0x01)       */
#define CS4304_ASP_CFG_SECONDARY_I2S 0x0001
/* SIGNAL_PATH_CFG: I2S format, normal channel order
 * Bits[2:0] = ASP_FORMAT: 000 = I2S Mode, 001 = Left-Justified Mode, 110/111 = TDM */
#define CS4304_SIGNAL_PATH_I2S       0x0000
/* OUT_ENABLES: enable both OUT1-4 DAC outputs (0x0F) and ASP inputs (0xF0)  */
#define CS4304_OUT_EN_ALL            0x00FF
#define CS4304_OUT_EN_CH1            0x0001
/* CHIP_ENABLE bit0                                                            */
#define CS4304_GLOBAL_EN             0x0001
/* OUT_VOL_CTRLx bit15                                                         */
#define CS4304_OUT_MUTE_BIT          0x8000
/* OUT_VOL_CTRL5 bit0                                                          */
#define CS4304_OUT_VU                0x0001

/* Volume: attenuation in 0.5 dB steps, 0x00 = 0 dB … 0xFF = −127.5 dB (§4.5.2) */
#define CS4304_VOL_0DB               0x00
#define CS4304_VOL_MIN               0xFF
#define CS4304_ATTEN_DB(db)          ((uint8_t)((db) * 2))   /* e.g. CS4304_ATTEN_DB(20) = −20 dB */

/** Initial attenuation applied by CS4304_Init.
  * −20 dB is a safe bring-up level for the output op-amp stage.
  * Host (USB) volume is applied in the sample data by the OS, so playback
  * level control still works; set to CS4304_VOL_0DB for full-scale tests. */
#define CS4304_INIT_ATTEN            CS4304_VOL_0DB  /* full scale; M031N level set by external 10k/1k divider */

/* Timing (Table 3-10: tRLPW ≥ 1 ms, tIRS ≤ 5 ms) ------------------------------*/
#define CS4304_RESET_PULSE_MS        2
#define CS4304_RESET_RECOVERY_MS     6
#define CS4304_I2C_TIMEOUT           100

/* Exported functions ---------------------------------------------------------*/

/** Scan the bus for the first ACKing device (8-bit addr), 0 if none. */
uint16_t CS4304_ScanBus(I2C_HandleTypeDef *hi2c);

/** Full init: HW reset → address probe/scan → SW_RESET → verify DEVID →
  * configure clocking/ASP (while GLOBAL_EN=0) → unmute, set volume, OUT_VU →
  * GLOBAL_EN=1. Output becomes active after the ~1 s startup delay. */
HAL_StatusTypeDef CS4304_Init(CS4304_HandleTypeDef *hcs4304);

HAL_StatusTypeDef CS4304_DeInit(CS4304_HandleTypeDef *hcs4304);
HAL_StatusTypeDef CS4304_Reset(CS4304_HandleTypeDef *hcs4304);

/** 16-bit register access (address must be word-aligned). */
HAL_StatusTypeDef CS4304_ReadReg(CS4304_HandleTypeDef *hcs4304, uint16_t reg, uint16_t *pData);
HAL_StatusTypeDef CS4304_WriteReg(CS4304_HandleTypeDef *hcs4304, uint16_t reg, uint16_t data);

/** Mute all channels (1) / unmute (0). Latched via OUT_VU. */
HAL_StatusTypeDef CS4304_SetMute(CS4304_HandleTypeDef *hcs4304, uint8_t mute);

/** Per-channel mute bitmask: bit0 = Ch1 … bit3 = Ch4. Latched via OUT_VU. */
HAL_StatusTypeDef CS4304_SetChannelMute(CS4304_HandleTypeDef *hcs4304, uint8_t mask);

/** Attenuation on all channels: 0x00 = 0 dB … 0xFF = −127.5 dB. */
HAL_StatusTypeDef CS4304_SetVolume(CS4304_HandleTypeDef *hcs4304, uint8_t atten);

/** Per-channel attenuation, channel = '1'..'4' or 'A'..'D'.
  * NOTE: overwritten by the next master volume change; for a persistent
  * per-channel offset use CS4304_SetChannelTrim instead. */
HAL_StatusTypeDef CS4304_SetChannelVolume(CS4304_HandleTypeDef *hcs4304, char channel, uint8_t atten);

/** Persistent per-channel trim: EXTRA attenuation (0.5 dB steps) applied on
  * top of the master volume, e.g. CS4304_ATTEN_DB(12) = −12 dB relative.
  * channel = '1'..'4' or 'A'..'D'. Re-applies volumes immediately. */
HAL_StatusTypeDef CS4304_SetChannelTrim(CS4304_HandleTypeDef *hcs4304, char channel, uint8_t trim);

CS4304_StateTypeDef CS4304_GetState(CS4304_HandleTypeDef *hcs4304);

#ifdef __cplusplus
}
#endif

#endif /* __CS4304_H */
