/**
  ******************************************************************************
  * @file    cs4304.c
  * @brief   CS4304S 4-Channel DAC Driver (I2C software-control mode)
  *
  *          Init sequence (§4.3):
  *            1. RESET low ≥ 1 ms, release, wait ≤ 5 ms (control port active)
  *            2. Resolve I2C address (CONFIG5-strapped; bus scan fallback)
  *            3. SW_RESET (0x5A00 → reg 0x0022)
  *            4. Verify DEVID (0x43B4)
  *            5. GLOBAL_EN = 0, then write clocking + ASP registers
  *               (they latch only on the GLOBAL_EN rising edge)
  *            6. GLOBAL_EN = 1 → output active after ~1 s startup delay
  *            7. Enable outputs, unmute + set volume, latch with OUT_VU
  *               (after GLOBAL_EN, matching official Cirrus scripts)
  ******************************************************************************
  */

#include "cs4304.h"

/* Private ---------------------------------------------------------------------*/

static void CS4304_AssertReset(void)
{
  HAL_GPIO_WritePin(DAC_RST_GPIO_Port, DAC_RST_Pin, GPIO_PIN_RESET);
}

static void CS4304_ReleaseReset(void)
{
  HAL_GPIO_WritePin(DAC_RST_GPIO_Port, DAC_RST_Pin, GPIO_PIN_SET);
}

/** Write one channel's OUT_VOL_CTRL register from mute flag + attenuation. */
static HAL_StatusTypeDef CS4304_WriteVol(CS4304_HandleTypeDef *h, uint16_t reg,
                                         uint8_t mute, uint8_t atten)
{
  uint16_t val = (mute ? CS4304_OUT_MUTE_BIT : 0x0000) | atten;
  return CS4304_WriteReg(h, reg, val);
}

/** Latch pending volume/mute changes (OUT_VU, write-only). */
static HAL_StatusTypeDef CS4304_LatchVolume(CS4304_HandleTypeDef *h)
{
  return CS4304_WriteReg(h, CS4304_REG_OUT_VOL_CTRL5, CS4304_OUT_VU);
}

/** Effective attenuation for one channel: master volume (if the channel
  * follows it, per MasterMask) plus per-channel trim, clamped. */
static uint8_t CS4304_ChanAtten(const CS4304_HandleTypeDef *h, uint8_t ch)
{
  uint32_t base  = ((h->MasterMask >> ch) & 1u) ? h->Volume : 0u;
  uint32_t total = base + (uint32_t)h->Trim[ch];
  return (total > 0xFFu) ? 0xFFu : (uint8_t)total;
}

/** Write all four volume registers (master volume + per-channel trim) + latch. */
static HAL_StatusTypeDef CS4304_ApplyVolume(CS4304_HandleTypeDef *h)
{
  static const uint16_t vol_regs[4] = {
    CS4304_REG_OUT_VOL_CTRL1_0, CS4304_REG_OUT_VOL_CTRL1_1,
    CS4304_REG_OUT_VOL_CTRL2_0, CS4304_REG_OUT_VOL_CTRL2_1
  };
  HAL_StatusTypeDef status;

  for (uint8_t i = 0; i < 4; i++)
  {
    status = CS4304_WriteVol(h, vol_regs[i], h->IsMuted, CS4304_ChanAtten(h, i));
    if (status != HAL_OK) return status;
  }

  return CS4304_LatchVolume(h);
}

/* Exported --------------------------------------------------------------------*/

uint16_t CS4304_ScanBus(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == NULL)
  {
    return 0;
  }

  for (uint8_t addr7 = 0x08; addr7 <= 0x77; addr7++)
  {
    if (HAL_I2C_IsDeviceReady(hi2c, (uint16_t)(addr7 << 1), 2, 10) == HAL_OK)
    {
      return (uint16_t)(addr7 << 1);
    }
  }
  return 0;
}

HAL_StatusTypeDef CS4304_ReadReg(CS4304_HandleTypeDef *hcs4304, uint16_t reg, uint16_t *pData)
{
  uint8_t buf[2];
  HAL_StatusTypeDef status;

  if (hcs4304 == NULL || hcs4304->hi2c == NULL || pData == NULL || hcs4304->DevAddr == 0)
  {
    return HAL_ERROR;
  }

  status = HAL_I2C_Mem_Read(hcs4304->hi2c, hcs4304->DevAddr,
                            reg, I2C_MEMADD_SIZE_16BIT,
                            buf, 2, CS4304_I2C_TIMEOUT);
  if (status == HAL_OK)
  {
    *pData = ((uint16_t)buf[0] << 8) | buf[1];  /* MSB first */
  }
  return status;
}

HAL_StatusTypeDef CS4304_WriteReg(CS4304_HandleTypeDef *hcs4304, uint16_t reg, uint16_t data)
{
  uint8_t buf[2];

  if (hcs4304 == NULL || hcs4304->hi2c == NULL || hcs4304->DevAddr == 0)
  {
    return HAL_ERROR;
  }

  buf[0] = (uint8_t)(data >> 8);   /* MSB first */
  buf[1] = (uint8_t)(data & 0xFF);

  return HAL_I2C_Mem_Write(hcs4304->hi2c, hcs4304->DevAddr,
                           reg, I2C_MEMADD_SIZE_16BIT,
                           buf, 2, CS4304_I2C_TIMEOUT);
}

HAL_StatusTypeDef CS4304_Init(CS4304_HandleTypeDef *hcs4304)
{
  HAL_StatusTypeDef status;

  if (hcs4304 == NULL || hcs4304->hi2c == NULL)
  {
    return HAL_ERROR;
  }

  hcs4304->Volume  = CS4304_INIT_ATTEN;
  hcs4304->Trim[0] = 0;
  hcs4304->Trim[1] = 0;
  hcs4304->Trim[2] = 0;
  hcs4304->Trim[3] = 0;
  hcs4304->MasterMask = 0x0F;  /* all channels follow master volume */
  hcs4304->IsMuted = 0;
  hcs4304->DevId   = 0;
  hcs4304->State   = CS4304_STATE_RESET;

  /* 1. Hardware reset */
  CS4304_AssertReset();
  HAL_Delay(CS4304_RESET_PULSE_MS);
  CS4304_ReleaseReset();
  HAL_Delay(CS4304_RESET_RECOVERY_MS);

  /* 2. Resolve I2C address (CONFIG5-strapped; scan as fallback) */
  if (hcs4304->DevAddr == 0 ||
      HAL_I2C_IsDeviceReady(hcs4304->hi2c, hcs4304->DevAddr, 3,
                            CS4304_I2C_TIMEOUT) != HAL_OK)
  {
    hcs4304->DevAddr = CS4304_ScanBus(hcs4304->hi2c);
    if (hcs4304->DevAddr == 0)
    {
      hcs4304->State = CS4304_STATE_ERROR;
      return HAL_ERROR;
    }
  }

  /* 3. Software reset to defaults */
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_SW_RESET, CS4304_SW_RESET_KEY);
  HAL_Delay(2);

  /* 4. Verify DEVID */
  status = CS4304_ReadReg(hcs4304, CS4304_REG_DEVID, &hcs4304->DevId);
  if (status != HAL_OK || hcs4304->DevId != CS4304_DEVID_VALUE)
  {
    hcs4304->State = CS4304_STATE_ERROR;
    return HAL_ERROR;
  }

  /* 5. Configure while GLOBAL_EN = 0 (clocking/ASP latch on its rising edge)
   *    - PLL enabled, MCLK reference @ 24.576 MHz (= 256·fs at 96 kHz)
   *    - Sample rate 96 kHz (CS4304_SAMPLE_RATE_ACTIVE)
   *    - ASP Secondary Mode, I2S format, BCLK non-inverted, normal order   */
  status = CS4304_WriteReg(hcs4304, CS4304_REG_CHIP_ENABLE, 0x0000);
  if (status != HAL_OK) { hcs4304->State = CS4304_STATE_ERROR; return status; }

  (void)CS4304_WriteReg(hcs4304, CS4304_REG_CLK_CFG_0, CS4304_CLK_CFG_0_PLL_MCLK_24M576);
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_CLK_CFG_1, CS4304_SAMPLE_RATE_ACTIVE);
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_ASP_CFG, CS4304_ASP_CFG_SECONDARY_I2S);
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_SIGNAL_PATH_CFG, CS4304_SIGNAL_PATH_I2S);
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_OUT_RAMP_SUM, 0x0022);
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_OUT_FILTER, 0x0200);

  /* 6. GLOBAL_EN = 1 — latches clocking/ASP config.
   *    Note: output becomes active after the DAC startup delay (~1 s
   *    default, STARTUP_DELAY register) once valid clocks are present.    */
  status = CS4304_WriteReg(hcs4304, CS4304_REG_CHIP_ENABLE, CS4304_GLOBAL_EN);
  if (status != HAL_OK) { hcs4304->State = CS4304_STATE_ERROR; return status; }

  /* 7. Enable all four DAC paths; unmute with initial attenuation.
   *    Per the official Cirrus bring-up scripts, OUT_ENABLES / volume /
   *    OUT_VU are written AFTER GLOBAL_EN = 1.
   *    (Channels default to MUTED — unmute + OUT_VU is mandatory.)        */
  (void)CS4304_WriteReg(hcs4304, CS4304_REG_OUT_ENABLES, CS4304_OUT_EN_ALL);
  status = CS4304_ApplyVolume(hcs4304);
  if (status != HAL_OK) { hcs4304->State = CS4304_STATE_ERROR; return status; }

  hcs4304->State = CS4304_STATE_READY;
  return HAL_OK;
}

HAL_StatusTypeDef CS4304_DeInit(CS4304_HandleTypeDef *hcs4304)
{
  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  if (hcs4304->hi2c != NULL && hcs4304->DevAddr != 0)
  {
    (void)CS4304_WriteReg(hcs4304, CS4304_REG_CHIP_ENABLE, 0x0000);
  }

  CS4304_AssertReset();
  hcs4304->State = CS4304_STATE_RESET;
  return HAL_OK;
}

HAL_StatusTypeDef CS4304_Reset(CS4304_HandleTypeDef *hcs4304)
{
  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  CS4304_AssertReset();
  HAL_Delay(CS4304_RESET_PULSE_MS);
  CS4304_ReleaseReset();
  HAL_Delay(CS4304_RESET_RECOVERY_MS);

  /* Registers are back to defaults — full re-init required. */
  hcs4304->State   = CS4304_STATE_RESET;
  hcs4304->IsMuted = 0;
  return HAL_OK;
}

HAL_StatusTypeDef CS4304_SetMute(CS4304_HandleTypeDef *hcs4304, uint8_t mute)
{
  HAL_StatusTypeDef status;

  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  hcs4304->IsMuted = mute ? 1 : 0;
  status = CS4304_ApplyVolume(hcs4304);
  if (status == HAL_OK)
  {
    hcs4304->State = mute ? CS4304_STATE_MUTED : CS4304_STATE_PLAYING;
  }
  return status;
}

HAL_StatusTypeDef CS4304_SetChannelMute(CS4304_HandleTypeDef *hcs4304, uint8_t mask)
{
  static const uint16_t vol_regs[4] = {
    CS4304_REG_OUT_VOL_CTRL1_0, CS4304_REG_OUT_VOL_CTRL1_1,
    CS4304_REG_OUT_VOL_CTRL2_0, CS4304_REG_OUT_VOL_CTRL2_1
  };
  HAL_StatusTypeDef status;

  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  for (uint8_t i = 0; i < 4; i++)
  {
    status = CS4304_WriteVol(hcs4304, vol_regs[i],
                             (mask >> i) & 1u, CS4304_ChanAtten(hcs4304, i));
    if (status != HAL_OK) return status;
  }
  return CS4304_LatchVolume(hcs4304);
}

HAL_StatusTypeDef CS4304_SetVolume(CS4304_HandleTypeDef *hcs4304, uint8_t atten)
{
  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  hcs4304->Volume = atten;
  return CS4304_ApplyVolume(hcs4304);
}

HAL_StatusTypeDef CS4304_SetChannelVolume(CS4304_HandleTypeDef *hcs4304, char channel, uint8_t atten)
{
  HAL_StatusTypeDef status;
  uint16_t reg;

  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  switch (channel)
  {
    case 'A': case 'a': case '1': reg = CS4304_REG_OUT_VOL_CTRL1_0; break;
    case 'B': case 'b': case '2': reg = CS4304_REG_OUT_VOL_CTRL1_1; break;
    case 'C': case 'c': case '3': reg = CS4304_REG_OUT_VOL_CTRL2_0; break;
    case 'D': case 'd': case '4': reg = CS4304_REG_OUT_VOL_CTRL2_1; break;
    default: return HAL_ERROR;
  }

  status = CS4304_WriteVol(hcs4304, reg, hcs4304->IsMuted, atten);
  if (status == HAL_OK)
  {
    status = CS4304_LatchVolume(hcs4304);
  }
  return status;
}

HAL_StatusTypeDef CS4304_SetChannelTrim(CS4304_HandleTypeDef *hcs4304, char channel, uint8_t trim)
{
  uint8_t idx;

  if (hcs4304 == NULL)
  {
    return HAL_ERROR;
  }

  switch (channel)
  {
    case 'A': case 'a': case '1': idx = 0; break;
    case 'B': case 'b': case '2': idx = 1; break;
    case 'C': case 'c': case '3': idx = 2; break;
    case 'D': case 'd': case '4': idx = 3; break;
    default: return HAL_ERROR;
  }

  hcs4304->Trim[idx] = trim;
  return CS4304_ApplyVolume(hcs4304);
}

CS4304_StateTypeDef CS4304_GetState(CS4304_HandleTypeDef *hcs4304)
{
  if (hcs4304 == NULL)
  {
    return CS4304_STATE_ERROR;
  }
  return hcs4304->State;
}
