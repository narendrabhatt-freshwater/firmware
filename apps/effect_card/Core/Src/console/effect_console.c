/**
 ******************************************************************************
 * @file    effect_console.c
 * @brief   Effect Card RS485 + USB CDC console, ADC bring-up, LED flash.
 *
 * Extracted from main.c — behavior-preserving. SAI→USB capture stays in main.
 ******************************************************************************
 */

#include "effect_console.h"

#include "main.h"
#include "gpio.h"
#include "i2c.h"
#include "usart.h"
#include "usb_app.h"

#include <stdio.h>
#include <string.h>

/* ---- Multi-drop RS485 card addressing ---- */
#define RS485_CARD_ID 'e'        /* 'c' = Channel Card, 'e' = Effect Card */
#define RS485_BROADCAST_ID '*'   /* broadcast prefix */
#define RS485_BUS_TIMEOUT_MS 250 /* max wait for bus to become free */
#define RS485_ECHO 0             /* default off: midi_host bursts at wire speed */

static uint8_t led_show_on = 1; /* LED flashing enable flag */
/** Runtime RS485 keystroke echo (bus re-TX). Default from RS485_ECHO.
 * midi_host turns this off for the session so Channel note frames can
 * burst at wire speed without colliding with our echo. */
static uint8_t rs485_echo = RS485_ECHO;

/** RS485 bus-aware transmit.
 *
 * Multi-drop protocol: two MCUs share the same transceiver (SN65HVD75).
 * Both PE1 (RS485_CTL = DE/RE̅) and PB9 (UART4_TX) are wired in parallel
 * with the other MCU's pins, so BOTH must be tri-stated (Hi-Z input) when
 * this card is not transmitting — otherwise the two push-pull drivers
 * fight each other and DE never rises.
 *
 * Idle state:  CTL = input (external 10 k pull-down keeps DE low),
 *              TX  = input with pull-up (keeps the shared D net at mark).
 * Transmit:    check CTL reads LOW (bus free), then TX -> UART AF,
 *              CTL -> push-pull output driven HIGH, send, and release.
 *
 * All outgoing text is prefixed with the card tag [E] so the host/other
 * cards know which MCU responded.
 */
#define RS485_TAG "[E] " /* response prefix tag for Effect Card */

/** Tri-state CTL and TX so the other MCU can use the bus (idle state). */
static void RS485_BusRelease(void) {
  GPIO_InitTypeDef gi = {0};

  /* Actively drive DE low first (the other card's CTL is Hi-Z input, so
   * no contention) — a fast clean falling edge instead of the slow 10 k
   * pull-down decay.  TX is still driving mark while DE falls, so the
   * transceiver can't clock out garbage during the turnaround. */
  HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_RESET);
  for (volatile uint32_t i = 0; i < 300; i++) {
  }

  gi.Pin = RS485_CTL_Pin;
  gi.Mode = GPIO_MODE_INPUT;
  gi.Pull = GPIO_NOPULL; /* external 10 k pull-down keeps DE low */
  HAL_GPIO_Init(RS485_CTL_GPIO_Port, &gi);

  gi.Pin = GPIO_PIN_9; /* PB9 = UART4_TX, shared D net */
  gi.Mode = GPIO_MODE_INPUT;
  gi.Pull = GPIO_PULLUP; /* keep D at mark (idle) level */
  HAL_GPIO_Init(GPIOB, &gi);
}

/** Take the bus: TX back to UART AF, CTL driven HIGH (transmit mode). */
static void RS485_BusAcquire(void) {
  GPIO_InitTypeDef gi = {0};

  gi.Pin = GPIO_PIN_9; /* PB9 = UART4_TX */
  gi.Mode = GPIO_MODE_AF_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  gi.Alternate = GPIO_AF8_UART4;
  HAL_GPIO_Init(GPIOB, &gi);

  HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_SET);
  gi.Pin = RS485_CTL_Pin;
  gi.Mode = GPIO_MODE_OUTPUT_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  gi.Alternate = 0;
  HAL_GPIO_Init(RS485_CTL_GPIO_Port, &gi);

  /* DE enable settle time (~1 us) before the first start bit */
  for (volatile uint32_t i = 0; i < 300; i++) {
  }
}

/** Check whether the RS485 bus is free (CTL low = no one transmitting).
 * Our own CTL is Hi-Z input while idle, so this reads the real DE line. */
static inline uint8_t RS485_BusFree(void) {
  return (HAL_GPIO_ReadPin(RS485_CTL_GPIO_Port, RS485_CTL_Pin) ==
          GPIO_PIN_RESET);
}

/** Wait up to timeout_ms for the bus to become free.  Returns 1 if free. */
static uint8_t RS485_WaitBusFree(uint32_t timeout_ms) {
  uint32_t t0 = HAL_GetTick();
  while (!RS485_BusFree()) {
    if ((HAL_GetTick() - t0) >= timeout_ms)
      return 0; /* timed out — bus still busy */
  }
  return 1;
}

/** Transmit a raw string on the RS485 bus with collision avoidance.
 * Waits for the bus to be free, asserts DE, sends data, releases DE.
 * Returns 0 on success, -1 if the bus was busy (timeout). */
static int RS485_Send(const char *s) {
  if (!RS485_WaitBusFree(RS485_BUS_TIMEOUT_MS))
    return -1; /* bus occupied — drop this message */

  RS485_BusAcquire();

  HAL_UART_Transmit(&huart4, (const uint8_t *)s, (uint16_t)strlen(s), 200);

  RS485_BusRelease();
  return 0;
}

/* When a command arrives over the USB CDC console, replies must go back
 * over USB instead of the RS485 bus.  Console_ExecFromUSB() sets this
 * for the duration of the command. */
static uint8_t console_via_usb = 0;

/** Send a tagged response — see Channel RS485_Reply (1 ms DE turnaround). */
void EffectConsole_Reply(const char *s) {
  char frame[96];
  size_t tag_len;
  size_t body_len;
  size_t n;

  if (console_via_usb) {
    USB_CDC_WriteStr(s);
    return;
  }

  tag_len = strlen(RS485_TAG);
  body_len = strlen(s);
  if (tag_len + body_len >= sizeof(frame)) {
    return;
  }
  memcpy(frame, RS485_TAG, tag_len);
  memcpy(frame + tag_len, s, body_len);
  n = tag_len + body_len;

  RS485_BusAcquire();
  (void)HAL_UART_Transmit(&huart4, (const uint8_t *)frame, (uint16_t)n, 30);
  RS485_BusRelease();
}

/* ---------------- Effect Card control console (RS485) ---------------- */

static void Console_Help(void) {
  /* One tagged line — leading \\r\\n would make the host see bare "[E]". */
  EffectConsole_Reply("ok: s | v 0|1 | l/lr/ly 0|1 | a 0|1 | i2c | "
                      "ai | ar n reg | aw n reg v | u 1..8 | ec 0|1\r\n");
}

static void Console_Status(void) {
  char b[96];
  GPIO_PinState en = HAL_GPIO_ReadPin(EN_48V_GPIO_Port, EN_48V_Pin);
  GPIO_PinState pg = HAL_GPIO_ReadPin(PG_48V_GPIO_Port, PG_48V_Pin);
  GPIO_PinState aud = HAL_GPIO_ReadPin(AUDIO_EN_GPIO_Port, AUDIO_EN_Pin);

  snprintf(b, sizeof b, "ok: v %u pg %u a %u l %u ec %u\r\n",
           (en == GPIO_PIN_SET) ? 1u : 0u, (pg == GPIO_PIN_SET) ? 1u : 0u,
           (aud == GPIO_PIN_SET) ? 1u : 0u, (unsigned)led_show_on,
           (unsigned)rs485_echo);
  EffectConsole_Reply(b);
}

/* ---- TLV320ADC6140 register access (both ADCs on I2C2) ----
 * ADC1: ADDR1:ADDR0 = 00 -> 7-bit 0x4C   (SAI1_A side)
 * ADC2: ADDR1:ADDR0 = 01 -> 7-bit 0x4D   (SAI1_B side)
 * All registers used here live in page 0 (the power-on page).  To touch
 * paged registers, switch pages manually: adc wr <n> 00 <page>. */
#define ADC1_ADDR7 0x4CU
#define ADC2_ADDR7 0x4DU

static uint8_t ADC_Addr7(unsigned chip) {
  return (chip == 2) ? ADC2_ADDR7 : ADC1_ADDR7;
}

static HAL_StatusTypeDef ADC_Wr(unsigned chip, uint8_t reg, uint8_t val) {
  return HAL_I2C_Mem_Write(&hi2c2, (uint16_t)(ADC_Addr7(chip) << 1), reg,
                           I2C_MEMADD_SIZE_8BIT, &val, 1, 20);
}

static HAL_StatusTypeDef ADC_Rd(unsigned chip, uint8_t reg, uint8_t *val) {
  return HAL_I2C_Mem_Read(&hi2c2, (uint16_t)(ADC_Addr7(chip) << 1), reg,
                          I2C_MEMADD_SIZE_8BIT, val, 1, 20);
}

/** Datasheet quick-start bring-up for one ADC: reset, wake, enable all
 * four input channels + ASI output slots, power up ADC and PLL.
 * MICBIAS is left off (line inputs); enable bit7 of 0x75 if needed. */
static void ADC_Init(unsigned chip) {
  char b[64];
  struct {
    uint8_t reg, val;
  } static const seq[] = {
      {0x01, 0x01}, /* SW_RESET: full register reset            */
      {0x02, 0x81}, /* SLEEP_CFG: internal AREG, exit sleep     */
      {0x07, 0x3C}, /* ASI_CFG0: TDM, 32-bit word, FSYNC pol
                       inverted to match SAI FS_ACTIVE_LOW      */
      {0x73, 0xF0}, /* IN_CH_EN: enable CH1..CH4                */
      {0x74, 0xF0}, /* ASI_OUT_CH_EN: CH1..CH4 to the ASI bus   */
      {0x75, 0x60}, /* PWR_CFG: power up ADC + PLL, MICBIAS off */
  };

  for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
    if (ADC_Wr(chip, seq[i].reg, seq[i].val) != HAL_OK) {
      snprintf(b, sizeof b, "err: adc%u no ack at reg 0x%02X\r\n", chip,
               seq[i].reg);
      EffectConsole_Reply(b);
      return;
    }
    HAL_Delay(2); /* reset/wake settle (datasheet: >=1 ms)      */
  }
  snprintf(b, sizeof b, "ok: adc%u (0x%02X) initialised\r\n", chip,
           ADC_Addr7(chip));
  EffectConsole_Reply(b);
}

/** Probe every legal 7-bit I2C address on I2C2 and report who ACKs.
 * Bring-up helper for the two ADCs; also catches missing pull-ups
 * (no device found + long runtime = SDA/SCL stuck). */
static void Console_I2CScan(void) {
  char b[48];
  uint8_t found = 0;

  EffectConsole_Reply("I2C2 scan (7-bit 0x08..0x77):\r\n");
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(a << 1), 2, 5) == HAL_OK) {
      snprintf(b, sizeof b, "  found: 0x%02X\r\n", a);
      EffectConsole_Reply(b);
      found++;
    }
  }
  snprintf(b, sizeof b, "done: %u device(s)\r\n", found);
  EffectConsole_Reply(b);
}

static void Console_Exec(char *line) {
  char b[64];

  if (line[0] == '\0') {
    return;
  }

  /* ---- help ---- */
  if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0 ||
      strcmp(line, "?") == 0) {
    Console_Help();
  }
  /* ---- s: status ---- */
  else if (strcmp(line, "s") == 0) {
    Console_Status();
  }
  /* ---- v 0|1 / v ---- */
  else if (strcmp(line, "v 1") == 0) {
    HAL_GPIO_WritePin(EN_48V_GPIO_Port, EN_48V_Pin, GPIO_PIN_SET);
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "v 0") == 0) {
    HAL_GPIO_WritePin(EN_48V_GPIO_Port, EN_48V_Pin, GPIO_PIN_RESET);
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "v") == 0) {
    GPIO_PinState en = HAL_GPIO_ReadPin(EN_48V_GPIO_Port, EN_48V_Pin);
    GPIO_PinState pg = HAL_GPIO_ReadPin(PG_48V_GPIO_Port, PG_48V_Pin);
    snprintf(b, sizeof b, "ok: v %u pg %u\r\n",
             (en == GPIO_PIN_SET) ? 1u : 0u, (pg == GPIO_PIN_SET) ? 1u : 0u);
    EffectConsole_Reply(b);
  }
  /* ---- l 0|1 ---- */
  else if (strcmp(line, "l 1") == 0) {
    led_show_on = 1;
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "l 0") == 0) {
    led_show_on = 0;
    EffectConsole_Reply("ok\r\n");
  }
  /* ---- lr 0|1 ---- */
  else if (strcmp(line, "lr 1") == 0) {
    led_show_on = 0;
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_SET);
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "lr 0") == 0) {
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_RESET);
    EffectConsole_Reply("ok\r\n");
  }
  /* ---- ly 0|1 ---- */
  else if (strcmp(line, "ly 1") == 0) {
    led_show_on = 0;
    HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin, GPIO_PIN_SET);
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "ly 0") == 0) {
    HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin, GPIO_PIN_RESET);
    EffectConsole_Reply("ok\r\n");
  }
  /* ---- a 0|1 ---- */
  else if (strcmp(line, "a 1") == 0) {
    HAL_GPIO_WritePin(AUDIO_EN_GPIO_Port, AUDIO_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(10); /* SHDNZ release: ADCs need ~1 ms before I2C access */
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "a 0") == 0) {
    HAL_GPIO_WritePin(AUDIO_EN_GPIO_Port, AUDIO_EN_Pin, GPIO_PIN_RESET);
    EffectConsole_Reply("ok\r\n");
  }
  /* ---- i2c ---- */
  else if (strcmp(line, "i2c") == 0) {
    Console_I2CScan();
  }
  /* ---- ai / ar / aw ---- */
  else if (strcmp(line, "ai") == 0) {
    ADC_Init(1);
    ADC_Init(2);
  } else if (strncmp(line, "ar ", 3) == 0) {
    unsigned chip, reg;
    uint8_t v;
    if (sscanf(line + 3, "%u %x", &chip, &reg) == 2 &&
        (chip == 1 || chip == 2) && reg <= 0xFF) {
      if (ADC_Rd(chip, (uint8_t)reg, &v) == HAL_OK) {
        snprintf(b, sizeof b, "ok: ar %u 0x%02X = 0x%02X\r\n", chip, reg, v);
      } else {
        snprintf(b, sizeof b, "err: ar %u no ack\r\n", chip);
      }
    } else {
      snprintf(b, sizeof b, "err:syntax\r\n");
    }
    EffectConsole_Reply(b);
  } else if (strncmp(line, "aw ", 3) == 0) {
    unsigned chip, reg, val;
    if (sscanf(line + 3, "%u %x %x", &chip, &reg, &val) == 3 &&
        (chip == 1 || chip == 2) && reg <= 0xFF && val <= 0xFF) {
      if (ADC_Wr(chip, (uint8_t)reg, (uint8_t)val) == HAL_OK) {
        snprintf(b, sizeof b, "ok: aw %u 0x%02X = 0x%02X\r\n", chip, reg, val);
      } else {
        snprintf(b, sizeof b, "err: aw %u no ack\r\n", chip);
      }
    } else {
      snprintf(b, sizeof b, "err:syntax\r\n");
    }
    EffectConsole_Reply(b);
  }
  /* ---- u <1..8> ---- */
  else if (line[0] == 'u' && (line[1] == '\0' || line[1] == ' ')) {
    unsigned ch;
    if (line[1] == '\0') {
      snprintf(b, sizeof b, "ok: u %u\r\n", usb_adc_ch);
      EffectConsole_Reply(b);
    } else if (sscanf(line + 1, "%u", &ch) == 1 && ch >= 1 && ch <= 8) {
      usb_adc_ch = (uint8_t)ch;
      EffectConsole_Reply("ok\r\n");
    } else {
      EffectConsole_Reply("err:syntax\r\n");
    }
  }
  /* ---- ec 0|1 / ec ---- */
  else if (strcmp(line, "ec 1") == 0) {
    rs485_echo = 1;
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "ec 0") == 0) {
    rs485_echo = 0;
    EffectConsole_Reply("ok\r\n");
  } else if (strcmp(line, "ec") == 0) {
    snprintf(b, sizeof b, "ok: ec %u\r\n", (unsigned)rs485_echo);
    EffectConsole_Reply(b);
  }
  /* ---- unknown ---- */
  else {
    EffectConsole_Reply("err:unknown\r\n");
    (void)line;
    (void)b;
  }
}

/** Check if a received line is addressed to this card.
 * Prefix format:  "X:command"  where X is a card ID letter.
 *   'C' = Channel Card,  'E' = Effect Card (us),  '*' = broadcast.
 * No prefix = broadcast (backward-compatible).
 *
 * If addressed to us, strips the prefix and returns 1.
 * If addressed to another card, returns 0 (ignore).
 * The stripped command is written back to `line`. */
static uint8_t RS485_IsForMe(char *line) {
  /* Check for "X:" prefix (at least 2 chars, second is ':') */
  if (line[0] != '\0' && line[1] == ':') {
    char id = line[0];
    if (id == RS485_CARD_ID || id == RS485_BROADCAST_ID) {
      /* Strip the "X:" prefix — shift the string left by 2 */
      memmove(line, line + 2, strlen(line + 2) + 1);
      return 1; /* addressed to us (or broadcast) */
    } else {
      return 0; /* addressed to another card — ignore */
    }
  }
  /* No prefix → treat as broadcast (backward-compatible) */
  return 1;
}

/** Console entry point for lines arriving over the USB CDC port.
 * Same parser and commands as RS485; replies are routed back to CDC. */
void Console_ExecFromUSB(char *line) {
  if (!RS485_IsForMe(line)) /* accept "e:", "*:" or bare commands */
    return;
  console_via_usb = 1;
  Console_Exec(line);
  console_via_usb = 0;
}

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * Messages addressed to other cards are silently dropped. */
static void Console_Poll(void) {
  static char cmd[48];
  static uint8_t idx = 0;
  uint8_t c;

  __HAL_UART_CLEAR_OREFLAG(&huart4);
  while (HAL_UART_Receive(&huart4, &c, 1, 0) == HAL_OK) {
    if (c == '\r' || c == '\n') {
      if (rs485_echo) {
        RS485_Send("\r\n");
      }
      cmd[idx] = '\0';

      /* Card-address filtering: only execute if addressed to us */
      if (RS485_IsForMe(cmd)) {
        /* No artificial holdoff — production requires echo off on this card. */
        Console_Exec(cmd);
      }
      /* else: message for another card — silently ignore */

      idx = 0;
    } else if (c == 0x08 || c == 0x7F) /* backspace */
    {
      if (idx > 0) {
        idx--;
        if (rs485_echo) {
          RS485_Send("\b \b");
        }
      }
    } else if (c >= 32 && c < 127 && idx < sizeof(cmd) - 1) {
      cmd[idx++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); /* lowercase */
      if (rs485_echo) {
        char e[2] = {(char)c, '\0'};
        RS485_Send(e); /* echo typing onto the shared bus */
      }
    }
  }
}

/** Non-blocking 2-LED alternating flash (LED_R and LED_Y, 200 ms per step)
 * so the console stays snappy. */
static void LED_Task(void) {
  static uint32_t t_next = 0;
  static uint8_t step = 0;

  if (!led_show_on) {
    /* When auto-flash is off, leave LEDs in whatever manual state they are */
    return;
  }
  if (HAL_GetTick() >= t_next) {
    t_next = HAL_GetTick() + 200;
    step ^= 1;
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin,
                      step ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_Y_GPIO_Port, LED_Y_Pin,
                      step ? GPIO_PIN_RESET : GPIO_PIN_SET);
  }
}


void EffectConsole_Init(void)
{
  /* Tri-state RS485_CTL (PE1) and UART4_TX (PB9) — both nets are shared
   * with the Channel Card MCU, so they must be Hi-Z whenever this card is
   * not transmitting.  RS485_BusAcquire()/Release() toggle them around
   * each transmission.  This overrides the push-pull/AF setup done by
   * MX_GPIO_Init()/MX_UART4_Init(). */
  RS485_BusRelease();

  /* --- Power-on LED startup sequence: blink R then Y --- */
  {
    const struct {
      GPIO_TypeDef *port;
      uint16_t pin;
    } leds[] = {
        {LED_R_GPIO_Port, LED_R_Pin},
        {LED_Y_GPIO_Port, LED_Y_Pin},
    };
    for (uint8_t i = 0; i < 2; i++) {
      HAL_GPIO_WritePin(leds[i].port, leds[i].pin, GPIO_PIN_SET);
      HAL_Delay(150);
      HAL_GPIO_WritePin(leds[i].port, leds[i].pin, GPIO_PIN_RESET);
    }
  }

  /* 48V off by default */
  HAL_GPIO_WritePin(EN_48V_GPIO_Port, EN_48V_Pin, GPIO_PIN_RESET);

  /* Enable the audio domain (ADC SHDNZ release).  The TLV320ADC6140s do
   * not respond on I2C while held in hardware shutdown, so this must be
   * high before any i2cscan/register access.  Datasheet asks for ~1 ms
   * after SHDNZ before the device is ready; give it 10. */
  HAL_GPIO_WritePin(AUDIO_EN_GPIO_Port, AUDIO_EN_Pin, GPIO_PIN_SET);
  HAL_Delay(10);

  /* Bring both ADCs up in TDM/32-bit mode before SAI DMA starts in main. */
  ADC_Init(1);
  ADC_Init(2);
}

void EffectConsole_Poll(void)
{
  Console_Poll();
  LED_Task();
}
