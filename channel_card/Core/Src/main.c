/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "i2s.h"
#include "sai.h"
#include "tim.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "cs4304.h"
#include "tim.h"
#include "usart.h"
#include "audio_bridge.h"
#include "usb_app.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ---- Multi-drop RS485 card addressing ---- */
#define RS485_CARD_ID 'c'       /* 'c' = Channel Card, 'e' = Effect Card */
#define RS485_BROADCAST_ID '*'  /* broadcast prefix */
#define RS485_BUS_TIMEOUT_MS 250 /* max wait for bus to become free */
#define RS485_ECHO 0 /* 0 = no typing echo (the Effect Card is the echo   \
                        master; exactly ONE card on the bus may echo) */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* CS4304 DAC handle — shared with usbd_audio_if.c */
CS4304_HandleTypeDef hcs4304;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/** RS485 bus-aware transmit.
 *
 * Multi-drop protocol: two MCUs share the same transceiver (SN65HVD75).
 * Both PB3 (RS485_CTL = DE/RE̅) and PC12 (UART5_TX) are wired in parallel
 * with the other MCU's pins, so BOTH must be tri-stated (Hi-Z input) when
 * this card is not transmitting — otherwise the two push-pull drivers
 * fight each other and DE never rises.
 *
 * Idle state:  CTL = input (external 10 k pull-down R34 keeps DE low),
 *              TX  = input with pull-up (keeps the shared D net at mark).
 * Transmit:    check CTL reads LOW (bus free), then TX -> UART AF,
 *              CTL -> push-pull output driven HIGH, send, and release.
 *
 * All outgoing text is prefixed with the card tag [C] so the host/other
 * cards know which MCU responded.
 */
#define RS485_TAG "[C] " /* response prefix tag for Channel Card */

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

  gi.Pin = GPIO_PIN_12; /* PC12 = UART5_TX, shared D net */
  gi.Mode = GPIO_MODE_INPUT;
  gi.Pull = GPIO_PULLUP; /* keep D at mark (idle) level */
  HAL_GPIO_Init(GPIOC, &gi);
}

/** Take the bus: TX back to UART AF, CTL driven HIGH (transmit mode). */
static void RS485_BusAcquire(void) {
  GPIO_InitTypeDef gi = {0};

  gi.Pin = GPIO_PIN_12; /* PC12 = UART5_TX */
  gi.Mode = GPIO_MODE_AF_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  gi.Alternate = GPIO_AF8_UART5;
  HAL_GPIO_Init(GPIOC, &gi);

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

/** Transmit a string on the RS485 bus with collision avoidance.
 * Waits for the bus to be free, asserts DE, sends data, releases DE.
 * Returns 0 on success, -1 if the bus was busy (timeout). */
static int __attribute__((unused)) RS485_Send(const char *s) {
  if (!RS485_WaitBusFree(RS485_BUS_TIMEOUT_MS))
    return -1; /* bus occupied — drop this message */

  RS485_BusAcquire();

  HAL_UART_Transmit(&huart5, (const uint8_t *)s, (uint16_t)strlen(s), 200);

  RS485_BusRelease();
  return 0;
}

/* When a command arrives over the USB CDC console, replies must go back
 * over USB instead of the RS485 bus.  Console_ExecFromUSB() sets this
 * for the duration of the command. */
static uint8_t console_via_usb = 0;

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied. */
static void RS485_Reply(const char *s) {
  if (console_via_usb) {
    USB_CDC_WriteStr(s);
    return;
  }
  if (!RS485_WaitBusFree(RS485_BUS_TIMEOUT_MS))
    return;

  RS485_BusAcquire();

  /* Send tag + payload in one DE assertion to keep the bus atomically */
  HAL_UART_Transmit(&huart5, (const uint8_t *)RS485_TAG,
                    (uint16_t)strlen(RS485_TAG), 50);
  HAL_UART_Transmit(&huart5, (const uint8_t *)s, (uint16_t)strlen(s), 200);

  RS485_BusRelease();
}

/* ---------------- Channel Card control console (RS485) ---------------- */

static uint8_t led_show_on = 1;

/* SCF clock duty (%): set by 'duty', used by 'scf'. 50 = original default. */
static unsigned int scf_duty_pct = 50;

/* ---- Switch table: maps short names to GPIO port/pin ---- */
typedef struct {
  const char *name;
  GPIO_TypeDef *port;
  uint16_t pin;
  uint8_t active_low; /* 1 = LOW enables the switch (ON) */
} SwitchDef_t;

static const SwitchDef_t switches[] = {
    {"hp", HP_SW_GPIO_Port, HP_SW_Pin, 1},
    {"bp", BP_SW_GPIO_Port, BP_SW_Pin, 1},
    {"lp", LP_SW_GPIO_Port, LP_SW_Pin, 1},
    {"vca", VCA_SW_GPIO_Port, VCA_SW_Pin, 1},
    {"bypass", BYPASS_SW_GPIO_Port, BYPASS_SW_Pin, 1},
    {"vcf", VCF_SW_GPIO_Port, VCF_SW_Pin, 1},
    {"scf", SCF_SW_GPIO_Port, SCF_SW_Pin, 1},
    {"hp_ctl", HP_CTL_GPIO_Port, HP_CTL_Pin, 0},
};
#define NUM_SWITCHES (sizeof(switches) / sizeof(switches[0]))

/* ---- PWM control table (TIM outputs for analog control) ---- */
typedef struct {
  const char *name;
  TIM_HandleTypeDef *htim;
  uint32_t channel;
  volatile uint32_t *ccr; /* pointer to TIMx->CCRy for direct duty set */
} PwmDef_t;

static const PwmDef_t pwm_outputs[] = {
    {"filter_ctl", &htim3, TIM_CHANNEL_1, &TIM3->CCR1},
};
#define NUM_PWM (sizeof(pwm_outputs) / sizeof(pwm_outputs[0]))

/* TIM3 input clock: APB1 timer clock = 275 MHz on this board
 * (SYSCLK 550 MHz / AHB÷2 = 275 MHz HCLK; APB1÷2 = 137.5 MHz;
 *  timer multiplier ×2 → 275 MHz). */
#define TIM_CLK_HZ 275000000UL

/** Logical ON state: for active-low switches, LOW = ON. */
static inline uint8_t SW_IsOn(const SwitchDef_t *sw) {
  GPIO_PinState raw = HAL_GPIO_ReadPin(sw->port, sw->pin);
  return sw->active_low ? (raw == GPIO_PIN_RESET) : (raw == GPIO_PIN_SET);
}

static void Console_Help(void) {
  RS485_Reply("\r\n"
              "**************************************************\r\n"
              "*            Channel Card Console                *\r\n"
              "**************************************************\r\n"
              "* help                 this page                 *\r\n"
              "* status               show all settings         *\r\n"
              "* sw                   show all switch states    *\r\n"
              "* sw <name> [on|off]   toggle/set switch         *\r\n"
              "*   names: hp bp lp vca bypass vcf scf hp_ctl    *\r\n"
              "*   (hp_ctl on = HP path clocked, off = filter)  *\r\n"
              "* scf <1..2000> kHz    SCF clock frequency, 0=off *\r\n"
              "* duty <1..99>         SCF clock duty % (dflt 50) *\r\n"
              "* dc <ch> <-100..100>  DC level %, ch=2..4        *\r\n"
              "* dczero <ch> <-99..99> calibrate 0V code        *\r\n"
              "* dctrim <ch> <-999..999> fine 0V trim (0.01%)   *\r\n"
              "* freq <ch> <Hz>       sine tone, ch=2..4        *\r\n"
              "* gain <ch> <dB>       DAC atten, ch=1..4        *\r\n"
              "* led <on|off>         LED flashing              *\r\n"
              "**************************************************\r\n"
              "Type a command and press Enter.\r\n\r\n");
}

static void Console_PrintSwitches(void) {
  char b[64];
  RS485_Reply("Switches:\r\n");
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    snprintf(b, sizeof b, "  %-10s %s\r\n", switches[i].name,
             SW_IsOn(&switches[i]) ? "ON" : "OFF");
    RS485_Reply(b);
  }
}

static void Console_PrintPWM(void) {
  char b[80];
  RS485_Reply("PWM outputs:\r\n");
  for (uint8_t i = 0; i < NUM_PWM; i++) {
    uint32_t arr = pwm_outputs[i].htim->Instance->ARR;
    uint32_t psc = pwm_outputs[i].htim->Instance->PSC;
    uint32_t ccr = *pwm_outputs[i].ccr;
    uint32_t pct = (arr > 0) ? (ccr * 100 / arr) : 0;
    uint32_t freq = TIM_CLK_HZ / ((psc + 1) * (arr + 1));
    snprintf(b, sizeof b, "  %-10s %lu%% @ %lu Hz\r\n", pwm_outputs[i].name,
             (unsigned long)pct, (unsigned long)freq);
    RS485_Reply(b);
  }
}

static const PwmDef_t *Pwm_Find(const char *name) {
  for (uint8_t i = 0; i < NUM_PWM; i++) {
    if (strcmp(name, pwm_outputs[i].name) == 0) {
      return &pwm_outputs[i];
    }
  }
  return NULL;
}

/** Set duty (0..100 %) and optionally frequency (freq_hz != 0) on a PWM
 * output. Maximises ARR for duty resolution; CCR rounded to nearest for
 * an accurate 50 % (SCF clock). Returns the actual output frequency. */
static uint32_t Pwm_Apply(const PwmDef_t *pw, unsigned int duty,
                          unsigned int freq_hz) {
  if (freq_hz >= 1 && freq_hz <= 2000000) {
    uint32_t total = TIM_CLK_HZ / freq_hz;
    uint32_t psc_val, arr_val;
    if (total <= 65536u) {
      psc_val = 0;
      arr_val = total - 1;
    } else {
      psc_val = total / 65536u;
      arr_val = (total / (psc_val + 1)) - 1;
    }
    pw->htim->Instance->PSC = (uint16_t)psc_val;
    pw->htim->Instance->ARR = (uint16_t)arr_val;
    pw->htim->Instance->EGR = TIM_EGR_UG; /* load shadow registers */
  }
  HAL_TIM_PWM_Start(pw->htim, pw->channel);
  uint32_t arr = pw->htim->Instance->ARR;
  *pw->ccr =
      ((arr + 1u) * duty + 50u) / 100u; /* nearest, exact 50% when even */
  uint32_t psc = pw->htim->Instance->PSC;
  return TIM_CLK_HZ / ((psc + 1) * (arr + 1));
}

static void Console_Status(void) {
  char b[96];

  RS485_Reply("\r\n**************** Status ****************\r\n");
  snprintf(b, sizeof b,
           "CH1: USB stream, trim -%u.%u dB (+ host volume -%u.%u dB)\r\n",
           hcs4304.Trim[0] / 2, (hcs4304.Trim[0] & 1) * 5, hcs4304.Volume / 2,
           (hcs4304.Volume & 1) * 5);
  RS485_Reply(b);
  for (uint8_t ch = 2; ch <= 4; ch++) {
    Audio_ChannelMode_t mode = Audio_GetChannelMode(ch);
    if (mode == AUDIO_MODE_DC) {
      snprintf(b, sizeof b,
               "CH%u: DC %d%% (zero %+d, inv %s), trim -%u.%u dB\r\n", ch,
               (int)Audio_GetDCLevel(ch), (int)Audio_GetDCZero(ch),
               Audio_GetDCInvert(ch) ? "on" : "off", hcs4304.Trim[ch - 1] / 2,
               (hcs4304.Trim[ch - 1] & 1) * 5);
    } else {
      snprintf(b, sizeof b, "CH%u: tone %lu Hz, trim -%u.%u dB\r\n", ch,
               (unsigned long)Audio_GetToneFreq(ch), hcs4304.Trim[ch - 1] / 2,
               (hcs4304.Trim[ch - 1] & 1) * 5);
    }
    RS485_Reply(b);
  }
  snprintf(b, sizeof b, "DC range cap: +/-%u%% FS | LED flashing: %s\r\n",
           Audio_GetDCLimit(), led_show_on ? "on" : "off");
  RS485_Reply(b);
  Console_PrintSwitches();
  Console_PrintPWM();
  RS485_Reply("****************************************\r\n");
}

static void Console_Exec(char *line) {
  unsigned int ch, val;
  int sval;
  char b[64];

  if (line[0] == '\0') {
    return;
  }
  if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0 ||
      strcmp(line, "?") == 0) {
    Console_Help();
  } else if (strcmp(line, "status") == 0 || strcmp(line, "s") == 0) {
    Console_Status();
  }
  /* ---- sw (no args): show all switch states ---- */
  else if (strcmp(line, "sw") == 0) {
    Console_PrintSwitches();
  }
  /* ---- sw <name> [on|off]: toggle or explicit set ---- */
  else if (strncmp(line, "sw ", 3) == 0) {
    char name[16] = {0};
    char action[8] = {0};
    int n = sscanf(line + 3, "%15s %7s", name, action);
    if (n >= 1) {
      /* Find the switch */
      const SwitchDef_t *sw = NULL;
      for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
        if (strcmp(name, switches[i].name) == 0) {
          sw = &switches[i];
          break;
        }
      }
      if (sw == NULL) {
        snprintf(b, sizeof b, "err: unknown switch '%s'\r\n", name);
        RS485_Reply(b);
      } else if (n == 1) /* toggle */
      {
        HAL_GPIO_TogglePin(sw->port, sw->pin);
        snprintf(b, sizeof b, "ok: %s -> %s\r\n", sw->name,
                 SW_IsOn(sw) ? "ON" : "OFF");
        RS485_Reply(b);
      } else if (strcmp(action, "on") == 0) {
        /* active-low: ON = RESET; active-high: ON = SET */
        HAL_GPIO_WritePin(sw->port, sw->pin,
                          sw->active_low ? GPIO_PIN_RESET : GPIO_PIN_SET);
        snprintf(b, sizeof b, "ok: %s -> ON\r\n", sw->name);
        RS485_Reply(b);
      } else if (strcmp(action, "off") == 0) {
        HAL_GPIO_WritePin(sw->port, sw->pin,
                          sw->active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);
        snprintf(b, sizeof b, "ok: %s -> OFF\r\n", sw->name);
        RS485_Reply(b);
      } else {
        RS485_Reply("err: sw <name> [on|off]\r\n");
      }
    } else {
      RS485_Reply("err: sw <name> [on|off]\r\n");
    }
  }
  /* ---- duty <pct>: SCF clock duty cycle (filter_ctl), keeps frequency ---- */
  else if (sscanf(line, "duty %u", &val) == 1) {
    if (val >= 1 && val <= 99) {
      scf_duty_pct = val;
      const PwmDef_t *pw = Pwm_Find("filter_ctl");
      if (pw != NULL) {
        uint32_t actual =
            Pwm_Apply(pw, scf_duty_pct, 0); /* 0 = keep current freq */
        snprintf(b, sizeof b, "ok: SCF duty %u%% @ %lu Hz\r\n", scf_duty_pct,
                 (unsigned long)actual);
        RS485_Reply(b);
      }
    } else {
      RS485_Reply("err: duty <1..99> %\r\n");
    }
  }
  /* ---- dctrim <ch> <x100>: fine zero trim, 0.01% units (digital trimmer) ----
   */
  else if (sscanf(line, "dctrim %u %d", &ch, &sval) == 2) {
    if (ch >= 2 && ch <= 4 && sval >= -999 && sval <= 999) {
      Audio_SetDCTrim((uint8_t)ch, (int16_t)sval);
      snprintf(b, sizeof b, "ok: CH%u fine trim %+d (x0.01%%)\r\n", ch, sval);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: dctrim <ch 2..4> <-999..999>\r\n");
    }
  }
  /* ---- dczero <ch> <pct>: calibrate the physical-0V code ---- */
  else if (sscanf(line, "dczero %u %d", &ch, &sval) == 2) {
    if (ch >= 2 && ch <= 4 && sval >= -99 && sval <= 99) {
      Audio_SetDCZero((uint8_t)ch, (int8_t)sval);
      snprintf(b, sizeof b,
               "ok: CH%u zero at code %+d; dc 0 = 0V, +/-100 symmetric\r\n", ch,
               sval);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: dczero <ch 2..4> <-99..99>\r\n");
    }
  }
  /* ---- dcinv <ch> on|off: per-channel DC sign inversion ---- */
  else if (sscanf(line, "dcinv %u %15s", &ch, b) == 2) {
    uint8_t on = (strcmp(b, "on") == 0);
    if (ch >= 2 && ch <= 4 && (on || strcmp(b, "off") == 0)) {
      Audio_SetDCInvert((uint8_t)ch, on);
      snprintf(b, sizeof b, "ok: CH%u dc sign %s\r\n", ch,
               on ? "inverted" : "normal");
      RS485_Reply(b);
    } else {
      RS485_Reply("err: dcinv <ch 2..4> <on|off>\r\n");
    }
  }
  /* ---- dcmax <pct>: symmetric DC range cap in % of full scale ---- */
  else if (sscanf(line, "dcmax %u", &val) == 1) {
    if (val >= 5 && val <= 100) {
      Audio_SetDCLimit((uint8_t)val);
      snprintf(b, sizeof b, "ok: DC range capped at +/-%u%% of full scale\r\n",
               val);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: dcmax <5..100>\r\n");
    }
  }
  /* ---- dc <ch> <percent>: set DC level on CH2..4 (signed) ---- */
  else if (sscanf(line, "dc %u %d", &ch, &sval) == 2) {
    if (ch >= 2 && ch <= 4 && sval >= -100 && sval <= 100) {
      Audio_SetDCLevel((uint8_t)ch, (int8_t)sval);
      snprintf(b, sizeof b, "ok: CH%u DC %d%%\r\n", ch, sval);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: dc <ch 2..4> <-100..100>\r\n");
    }
  }
  /* ---- pwm <name> <duty%> [freq_hz]: set PWM duty and optional freq ---- */
  else if (strncmp(line, "pwm ", 4) == 0) {
    char name[16] = {0};
    unsigned int duty = 0;
    unsigned int freq = 0;
    int n = sscanf(line + 4, "%15s %u %u", name, &duty, &freq);
    if (n >= 2 && duty <= 100) {
      const PwmDef_t *pw = Pwm_Find(name);
      if (pw == NULL) {
        snprintf(b, sizeof b, "err: unknown pwm '%s'\r\n", name);
        RS485_Reply(b);
      } else {
        uint32_t actual = Pwm_Apply(pw, duty, (n == 3) ? freq : 0);
        snprintf(b, sizeof b, "ok: %s -> %u%% @ %lu Hz\r\n", pw->name, duty,
                 (unsigned long)actual);
        RS485_Reply(b);
      }
    } else if (n >= 1 && name[0] != '\0') {
      RS485_Reply("err: pwm <name> <0..100> [freq_hz]\r\n");
    } else {
      Console_PrintPWM();
    }
  }
  /* ---- scf <freq_hz>: switched-cap filter clock, fixed 50% duty.
   *      scf 0 stops the PWM and parks the pin low. ---- */
  else if (sscanf(line, "scf %u", &val) == 1) {
    const PwmDef_t *pw = Pwm_Find("filter_ctl");
    if (pw == NULL) {
      /* table misconfigured — nothing to drive */
    } else if (val == 0) {
      /* Do NOT stop the timer: PWM_Stop releases the pin to Hi-Z and the
       * external network floats it (~4.6 V, undefined switches). Keep the
       * channel running at constant LOW: with the NPN open-collector
       * inverters ahead of both PNP shifters, PC6 low -> all 4066 at -5
       * (off) and the 1G00 output high -> DG441 off. */
      HAL_TIM_PWM_Start(pw->htim, pw->channel);
      *pw->ccr = 0; /* 0% = pin solid low */
      RS485_Reply("ok: SCF clock off (pin parked LOW, all switches off)\r\n");
    } else if (val >= 1 && val <= 2000) /* unit: kHz */
    {
      uint32_t actual = Pwm_Apply(pw, scf_duty_pct, val * 1000u);
      snprintf(b, sizeof b, "ok: SCF clock %lu Hz @ %u%%\r\n",
               (unsigned long)actual, scf_duty_pct);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: scf <1..2000> kHz, 0 = off\r\n");
    }
  } else if (sscanf(line, "gain %u %u", &ch, &val) == 2) {
    if (ch >= 1 && ch <= 4 && val <= 127) {
      CS4304_SetChannelTrim(&hcs4304, (char)('0' + ch), (uint8_t)(val * 2));
      snprintf(b, sizeof b, "ok: CH%u attenuation -%u dB\r\n", ch, val);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: gain <ch 1..4> <dB 0..127>\r\n");
    }
  } else if (sscanf(line, "freq %u %u", &ch, &val) == 2) {
    if (ch >= 2 && ch <= 4 && val >= 20 && val < 20000) {
      Audio_SetChannelMode((uint8_t)ch, AUDIO_MODE_TONE);
      Audio_SetToneFreq((uint8_t)ch, (uint32_t)val);
      snprintf(b, sizeof b, "ok: CH%u tone %u Hz\r\n", ch, val);
      RS485_Reply(b);
    } else {
      RS485_Reply("err: freq <ch 2..4> <Hz 20..19999>\r\n");
    }
  } else if (strcmp(line, "led on") == 0) {
    led_show_on = 1;
    RS485_Reply("ok: LED flashing on\r\n");
  } else if (strcmp(line, "led off") == 0) {
    led_show_on = 0;
    RS485_Reply("ok: LED flashing off\r\n");
  } else {
    snprintf(b, sizeof b, "unknown: '%s' (try 'help')\r\n", line);
    RS485_Reply(b);
  }
}

/** Check if a received line is addressed to this card.
 * Prefix format:  "X:command"  where X is a card ID letter.
 *   'C' = Channel Card (us),  'E' = Effect Card,  '*' = broadcast.
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
  if (!RS485_IsForMe(line)) /* accept "c:", "*:" or bare commands */
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

  __HAL_UART_CLEAR_OREFLAG(&huart5);
  while (HAL_UART_Receive(&huart5, &c, 1, 0) == HAL_OK) {
    if (c == '\r' || c == '\n') {
#if RS485_ECHO
      RS485_Send("\r\n");
#endif
      cmd[idx] = '\0';

      /* Card-address filtering: only execute if addressed to us */
      if (RS485_IsForMe(cmd)) {
#if !RS485_ECHO
        /* The echo master transmits its "\r\n" echo at the same instant
         * we received Enter — hold off briefly so our reply doesn't
         * collide with it (both would sample "bus free" simultaneously,
         * which CSMA cannot serialize). */
        HAL_Delay(3);
#endif
        Console_Exec(cmd);
      }
      /* else: message for another card — silently ignore */

      idx = 0;
    } else if (c == 0x08 || c == 0x7F) /* backspace */
    {
      if (idx > 0) {
        idx--;
#if RS485_ECHO
        RS485_Send("\b \b");
#endif
      }
    } else if (c >= 32 && c < 127 && idx < sizeof(cmd) - 1) {
      cmd[idx++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); /* lowercase */
#if RS485_ECHO
      char e[2] = {(char)c, '\0'};
      RS485_Send(e); /* echo typing */
#endif
    }
  }
}

/** Non-blocking 5-LED chaser (150 ms per step) so the console stays snappy. */
static void LED_Task(void) {
  static const GPIO_TypeDef *ports[5] = {LED_R_GPIO_Port, LED_Y_GPIO_Port,
                                         RGB_R_GPIO_Port, RGB_G_GPIO_Port,
                                         RGB_B_GPIO_Port};
  static const uint16_t pins[5] = {LED_R_Pin, LED_Y_Pin, RGB_R_Pin, RGB_G_Pin,
                                   RGB_B_Pin};
  static uint32_t t_next = 0;
  static uint8_t step = 0;

  if (!led_show_on) {
    for (uint8_t i = 0; i < 5; i++) {
      HAL_GPIO_WritePin((GPIO_TypeDef *)ports[i], pins[i], GPIO_PIN_RESET);
    }
    return;
  }
  if (HAL_GetTick() >= t_next) {
    t_next = HAL_GetTick() + 150;
    HAL_GPIO_WritePin((GPIO_TypeDef *)ports[step], pins[step], GPIO_PIN_RESET);
    step = (step + 1) % 5;
    HAL_GPIO_WritePin((GPIO_TypeDef *)ports[step], pins[step], GPIO_PIN_SET);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C5_Init();
  MX_SAI4_Init();
  MX_TIM3_Init();
  MX_TIM12_Init();
  MX_UART5_Init();
  MX_I2S1_Init();
  MX_I2S2_Init();
  MX_USB_OTG_HS_PCD_Init();
  /* USER CODE BEGIN 2 */

  /* USB is owned by TinyUSB (UAC2 speaker + CDC console) — see USB_APP/.
   * ST's MX_USB_DEVICE_Init() is gone with the USB_DEVICE middleware;
   * USB_App_Init() does the clocks, PHY power and NVIC itself.
   * Kept inside USER CODE so CubeMX regeneration preserves it. */
  USB_App_Init();


  /* Tri-state RS485_CTL (PB3) and UART5_TX (PC12) — both nets are shared
   * with the Effect Card MCU, so they must be Hi-Z whenever this card is
   * not transmitting.  RS485_BusAcquire()/Release() toggle them around
   * each transmission.  This overrides the push-pull/AF setup done by
   * MX_GPIO_Init()/MX_UART5_Init(). */
  RS485_BusRelease();

  /* --- Power-on LED startup sequence: flash all 5 LEDs one by one --- */
  {
    const GPIO_TypeDef *led_ports[] = {LED_R_GPIO_Port, LED_Y_GPIO_Port,
                                       RGB_R_GPIO_Port, RGB_G_GPIO_Port,
                                       RGB_B_GPIO_Port};
    const uint16_t led_pins[] = {LED_R_Pin, LED_Y_Pin, RGB_R_Pin, RGB_G_Pin,
                                 RGB_B_Pin};
    const uint8_t num_leds = 5;

    for (uint8_t i = 0; i < num_leds; i++) {
      HAL_GPIO_WritePin((GPIO_TypeDef *)led_ports[i], led_pins[i],
                        GPIO_PIN_SET);
      HAL_Delay(150);
      HAL_GPIO_WritePin((GPIO_TypeDef *)led_ports[i], led_pins[i],
                        GPIO_PIN_RESET);
    }
  }

  /* Initialize CS4304S DAC via I2C5.
   * CONFIG5 0R to GND -> I2C address 0x60 (DS1388F1 Table 4-17); the driver
   * falls back to a bus scan if that address does not ACK.
   * NON-FATAL: the channel-1 USB playback test must run even if the DAC
   * I2C link is down (LED_R stays on solid as an error indicator). */
  hcs4304.hi2c = &hi2c5;
  hcs4304.DevAddr = CS4304_I2C_ADDR_C5_GND_0R;

  if (CS4304_Init(&hcs4304) != HAL_OK) {
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_SET);
  }

  /* Only CH1 (USB audio) follows the host volume slider; CH2-CH4 (test
   * tones) stay at full scale (0 dB + their Trim, if any). */
  hcs4304.MasterMask = 0x01;
  CS4304_SetVolume(&hcs4304, hcs4304.Volume); /* re-apply with the mask */

  /* DAC is ready. USB audio will start when host begins streaming. */
  /* I2S DMA is started in AUDIO_Init_HS() when USB audio is activated. */

  /* Start I2S DMA for standalone playback */
  Audio_StartPlayback();

  /* Default DC level 0 (true 0 V via dczero calibration) for CH2-CH4 */
  Audio_SetDCLevel(2, 0);
  Audio_SetDCLevel(3, 0);
  Audio_SetDCLevel(4, 0);

  /* All analog path switches default OFF */
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    HAL_GPIO_WritePin(switches[i].port, switches[i].pin,
                      switches[i].active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }

  /* MX_TIM12_Init() reconfigured PB15 as TIM12_CH2 AF — override it back
   * to plain GPIO output so it can be toggled via the 'sw hp_ctl' command.
   * (Only filter_ctl on PC6 is a genuine PWM output.) */
  {
    GPIO_InitTypeDef gi = {0};
    gi.Pin = HP_CTL_Pin;
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(HP_CTL_GPIO_Port, &gi);
    HAL_GPIO_WritePin(HP_CTL_GPIO_Port, HP_CTL_Pin,
                      GPIO_PIN_RESET); /* LOW = hp_ctl off (filter mode) */
  }

  /* filter_ctl (TIM3_CH1, PC6): keep LOW at boot — PWM is started on
   * first 'pwm' console command so the pin stays quiet by default. */

  RS485_Reply("\r\n"
              "**************************************************\r\n"
              "*  Channel Card [C] ready  (multi-drop RS485)    *\r\n"
              "*  Type 'help' or 'c:help' for the console menu. *\r\n"
              "**************************************************\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    Console_Poll(); /* RS485 command console (see RS485_MANUAL.md) */
    USB_App_Task(); /* TinyUSB stack + CDC console */
    LED_Task();     /* non-blocking LED chaser */
    // HAL_Delay(10);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 3;
  RCC_OscInitStruct.PLL.PLLN = 67;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 10;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 1136;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CKPER;
  PeriphClkInitStruct.CkperClockSelection = RCC_CLKPSOURCE_HSE;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
