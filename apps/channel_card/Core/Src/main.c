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
#include "note_bank.h"
#include "usb_app.h"
#include <stdio.h>
#include <string.h>/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ---- Multi-drop RS485 card addressing ---- */
#define RS485_CARD_ID 'c'        /* 'c' = Channel Card, 'e' = Effect Card */
#define RS485_BROADCAST_ID '*'   /* broadcast prefix */
#define RS485_BUS_TIMEOUT_MS 250 /* max wait for bus to become free */
#define RS485_ECHO 0             /* 0 = no typing echo (the Effect Card is the echo \
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
static void RS485_BusRelease(void)
{
  GPIO_InitTypeDef gi = {0};

  /* Actively drive DE low first (the other card's CTL is Hi-Z input, so
   * no contention) — a fast clean falling edge instead of the slow 10 k
   * pull-down decay.  TX is still driving mark while DE falls, so the
   * transceiver can't clock out garbage during the turnaround. */
  HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_RESET);
  for (volatile uint32_t i = 0; i < 300; i++)
  {
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
static void RS485_BusAcquire(void)
{
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
  for (volatile uint32_t i = 0; i < 300; i++)
  {
  }
}

/** Check whether the RS485 bus is free (CTL low = no one transmitting).
 * Our own CTL is Hi-Z input while idle, so this reads the real DE line. */
static inline uint8_t RS485_BusFree(void)
{
  return (HAL_GPIO_ReadPin(RS485_CTL_GPIO_Port, RS485_CTL_Pin) ==
          GPIO_PIN_RESET);
}

/** Wait up to timeout_ms for the bus to become free.  Returns 1 if free. */
static uint8_t RS485_WaitBusFree(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  while (!RS485_BusFree())
  {
    if ((HAL_GetTick() - t0) >= timeout_ms)
      return 0; /* timed out — bus still busy */
  }
  return 1;
}

/** Transmit a string on the RS485 bus with collision avoidance.
 * Waits for the bus to be free, asserts DE, sends data, releases DE.
 * Returns 0 on success, -1 if the bus was busy (timeout). */
static int __attribute__((unused)) RS485_Send(const char *s)
{
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
static void RS485_Reply(const char *s)
{
  if (console_via_usb)
  {
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

/* Boot-time switch defaults only — console no longer exposes sw/pwm/etc. */
typedef struct
{
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

/* Console commands (RS485 + USB CDC). Console_Poll lowercases, so "N0"/"GAIN"
 * arrive as "n0"/"gain".
 *
 *   n0          — session defaults: bypass ON + gain 1 0
 *   n0..nf <Hz> — set note 0..15 frequency (0=off); summed onto CH1
 *   gain <ch> <dB> — CS4304 per-channel DAC atten (0..127 dB), ch 1..4 */
#define N0_DEFAULT_ATTEN_DB 0u

/** Bypass is active-low (LOW = ON). */
static void Console_SetBypassOn(void)
{
  HAL_GPIO_WritePin(BYPASS_SW_GPIO_Port, BYPASS_SW_Pin, GPIO_PIN_RESET);
}

/** Defaults for bare n0 / boot: dry path + CH1 DAC trim 0 dB (`gain 1 0`).
 * Frequency changes (`nX <Hz>`) do not touch gain or bypass. */
static void Console_ApplySessionDefaults(void)
{
  Console_SetBypassOn();
  CS4304_SetChannelTrim(&hcs4304, '1', (uint8_t)(N0_DEFAULT_ATTEN_DB * 2u));
}

/** Map hex digit '0'..'9','a'..'f' → 0..15; else 0xFF. */
static uint8_t Console_ParseNoteSlot(char hex_digit)
{
  if (hex_digit >= '0' && hex_digit <= '9')
  {
    return (uint8_t)(hex_digit - '0');
  }
  if (hex_digit >= 'a' && hex_digit <= 'f')
  {
    return (uint8_t)(10u + (uint8_t)(hex_digit - 'a'));
  }
  return 0xFFu;
}

/** Apply nX <Hz> after the slot is known. Replies ok:/err:. */
static void Console_SetNoteFreq(uint8_t note, double hz)
{
  char b[80];
  char tag = (note < 10u) ? (char)('0' + note) : (char)('a' + (note - 10u));

  if (hz <= 0.0)
  {
    NoteBank_SetFreq(note, 0.0);
    snprintf(b, sizeof b, "ok: N%c off\r\n", tag);
    RS485_Reply(b);
    return;
  }

  if (hz < 20.0 || hz >= 20000.0)
  {
    snprintf(b, sizeof b, "err: n%c <Hz> (0=off, else 20..19999.9)\r\n", tag);
    RS485_Reply(b);
    return;
  }

  NoteBank_SetFreq(note, hz);
  snprintf(b, sizeof b, "ok: N%c %.1f Hz\r\n", tag, hz);
  RS485_Reply(b);
}

static void Console_Exec(char *line)
{
  char b[80];
  double hz;
  unsigned int ch, val;
  uint8_t note;

  if (line[0] == '\0')
  {
    return;
  }

  /* ---- gain <ch> <dB>: CS4304 DAC atten (0.5 dB steps via trim×2) ---- */
  if (sscanf(line, "gain %u %u", &ch, &val) == 2)
  {
    if (ch >= 1 && ch <= 4 && val <= 127)
    {
      CS4304_SetChannelTrim(&hcs4304, (char)('0' + ch), (uint8_t)(val * 2u));
      snprintf(b, sizeof b, "ok: CH%u attenuation -%u dB\r\n", ch, val);
      RS485_Reply(b);
    }
    else
    {
      RS485_Reply("err: gain <ch 1..4> <dB 0..127>\r\n");
    }
    return;
  }

  /* ---- n0..nf: 16-voice note bank on CH1 ---- */
  if (line[0] != 'n' || line[1] == '\0')
  {
    RS485_Reply("err: commands are n0..nf <Hz> | gain <ch> <dB>\r\n");
    return;
  }

  note = Console_ParseNoteSlot(line[1]);
  if (note == 0xFFu || (line[2] != '\0' && line[2] != ' '))
  {
    RS485_Reply("err: commands are n0..nf <Hz> | gain <ch> <dB>\r\n");
    return;
  }

  /* Bare "n0" = session defaults only. Bare n1..nf are not session cmds. */
  if (line[2] == '\0')
  {
    if (note == 0u)
    {
      Console_ApplySessionDefaults();
      snprintf(b, sizeof b, "ok: bypass on, gain 1 -%u dB\r\n",
               (unsigned)N0_DEFAULT_ATTEN_DB);
      RS485_Reply(b);
    }
    else
    {
      RS485_Reply("err: n1..nf need <Hz> (bare n0 = session defaults)\r\n");
    }
    return;
  }

  if (sscanf(line + 3, "%lf", &hz) != 1)
  {
    snprintf(b, sizeof b, "err: n%c <Hz> (0=off, else 20..19999.9)\r\n",
             line[1]);
    RS485_Reply(b);
    return;
  }

  Console_SetNoteFreq(note, hz);
}

/** Check if a received line is addressed to this card.
 * Prefix format:  "X:command"  where X is a card ID letter.
 *   'C' = Channel Card (us),  'E' = Effect Card,  '*' = broadcast.
 * No prefix = broadcast (backward-compatible).
 *
 * If addressed to us, strips the prefix and returns 1.
 * If addressed to another card, returns 0 (ignore).
 * The stripped command is written back to `line`. */
static uint8_t RS485_IsForMe(char *line)
{
  /* Check for "X:" prefix (at least 2 chars, second is ':') */
  if (line[0] != '\0' && line[1] == ':')
  {
    char id = line[0];
    if (id == RS485_CARD_ID || id == RS485_BROADCAST_ID)
    {
      /* Strip the "X:" prefix — shift the string left by 2 */
      memmove(line, line + 2, strlen(line + 2) + 1);
      return 1; /* addressed to us (or broadcast) */
    }
    else
    {
      return 0; /* addressed to another card — ignore */
    }
  }
  /* No prefix → treat as broadcast (backward-compatible) */
  return 1;
}

/** Console entry point for lines arriving over the USB CDC port.
 * Same parser and commands as RS485; replies are routed back to CDC. */
void Console_ExecFromUSB(char *line)
{
  if (!RS485_IsForMe(line)) /* accept "c:", "*:" or bare commands */
    return;
  console_via_usb = 1;
  Console_Exec(line);
  console_via_usb = 0;
}

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * Messages addressed to other cards are silently dropped. */
static void Console_Poll(void)
{
  static char cmd[48];
  static uint8_t idx = 0;
  uint8_t c;

  __HAL_UART_CLEAR_OREFLAG(&huart5);
  while (HAL_UART_Receive(&huart5, &c, 1, 0) == HAL_OK)
  {
    if (c == '\r' || c == '\n')
    {
#if RS485_ECHO
      RS485_Send("\r\n");
#endif
      cmd[idx] = '\0';

      /* Card-address filtering: only execute if addressed to us */
      if (RS485_IsForMe(cmd))
      {
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
    }
    else if (c == 0x08 || c == 0x7F) /* backspace */
    {
      if (idx > 0)
      {
        idx--;
#if RS485_ECHO
        RS485_Send("\b \b");
#endif
      }
    }
    else if (c >= 32 && c < 127 && idx < sizeof(cmd) - 1)
    {
      cmd[idx++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); /* lowercase */
#if RS485_ECHO
      char e[2] = {(char)c, '\0'};
      RS485_Send(e); /* echo typing */
#endif
    }
  }
}

/** Non-blocking 5-LED chaser (150 ms per step) so the console stays snappy. */
static void LED_Task(void)
{
  static const GPIO_TypeDef *ports[5] = {LED_R_GPIO_Port, LED_Y_GPIO_Port,
                                         RGB_R_GPIO_Port, RGB_G_GPIO_Port,
                                         RGB_B_GPIO_Port};
  static const uint16_t pins[5] = {LED_R_Pin, LED_Y_Pin, RGB_R_Pin, RGB_G_Pin,
                                   RGB_B_Pin};
  static uint32_t t_next = 0;
  static uint8_t step = 0;

  if (!led_show_on)
  {
    for (uint8_t i = 0; i < 5; i++)
    {
      HAL_GPIO_WritePin((GPIO_TypeDef *)ports[i], pins[i], GPIO_PIN_RESET);
    }
    return;
  }
  if (HAL_GetTick() >= t_next)
  {
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

    for (uint8_t i = 0; i < num_leds; i++)
    {
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

  if (CS4304_Init(&hcs4304) != HAL_OK)
  {
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

  /* All analog path switches default OFF, then session defaults: bypass ON
   * and CH1 @ 0 dB so RS485 N0 tones are audible on the dry path. */
  for (uint8_t i = 0; i < NUM_SWITCHES; i++)
  {
    HAL_GPIO_WritePin(switches[i].port, switches[i].pin,
                      switches[i].active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  Console_ApplySessionDefaults();

  /* MX_TIM12_Init() reconfigured PB15 as TIM12_CH2 AF — override it back
   * to plain GPIO output so hp_ctl stays a GPIO. */
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

  /* filter_ctl (TIM3_CH1, PC6): keep LOW at boot. */

  RS485_Reply("\r\n"
              "**************************************************\r\n"
              "*  Channel Card [C] ready  (multi-drop RS485)    *\r\n"
              "*  n0..nf <Hz> | gain <ch> <dB>                  *\r\n"
              "*  enter/boot: bypass ON, gain 1 0               *\r\n"
              "**************************************************\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
  }

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_HSE;
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
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
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
  while (1)
  {
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
