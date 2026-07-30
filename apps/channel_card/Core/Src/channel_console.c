/**
 ******************************************************************************
 * @file    channel_console.c
 * @brief   Channel Card RS485 + USB CDC console, cpuload probe, LED chaser.
 *
 * Extracted from main.c (behavior-preserving). main.c owns bring-up +
 * hcs4304; this file owns the interactive console surface.
 ******************************************************************************
 */

#include "channel_console.h"

#include "main.h"
#include "usart.h"
#include "cs4304.h"
#include "audio_bridge.h"
#include "note_bank.h"
#include "uart5_rx.h"
#include "usb_app.h"

#include <stdio.h>
#include <string.h>

extern CS4304_HandleTypeDef hcs4304;
extern UART_HandleTypeDef huart5;

/* ---- Multi-drop RS485 card addressing ---- */
#define RS485_CARD_ID 'c'
#define RS485_BROADCAST_ID '*'
#define RS485_BUS_TIMEOUT_MS 250
#define RS485_ECHO 0


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
 *   n0                — session defaults: bypass ON + gain 1 0
 *   n0..nf <Hz> [sc]  — note freq; optional scale 0.0..1.0 (default 1.0)
 *   gain <ch> <dB>    — CS4304 per-channel DAC atten (0..127 dB), ch 1..4
 *   cpuload off|on|dma|queue — LED_Y (PB9) CPU-load probe (see README) */
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

/** Apply nX <Hz> [scale]. Scale defaults to 1.0 when omitted by caller. */
static void Console_SetNoteFreq(uint8_t note, double hz, double scale)
{
  char b[80];
  char tag = (note < 10u) ? (char)('0' + note) : (char)('a' + (note - 10u));

  if (hz <= 0.0)
  {
    NoteBank_SetFreq(note, 0.0, 0.0);
    snprintf(b, sizeof b, "ok: N%c off\r\n", tag);
    RS485_Reply(b);
    return;
  }

  if (hz < 20.0 || hz >= 20000.0)
  {
    snprintf(b, sizeof b,
             "err: n%c <Hz> [scale] (0=off, else 20..19999.9, scale 0..1)\r\n",
             tag);
    RS485_Reply(b);
    return;
  }

  if (scale < 0.0 || scale > 1.0)
  {
    snprintf(b, sizeof b, "err: n%c scale must be 0.0..1.0\r\n", tag);
    RS485_Reply(b);
    return;
  }

  NoteBank_SetFreq(note, hz, scale);
  snprintf(b, sizeof b, "ok: N%c %.1f Hz scale %.2f\r\n", tag, hz, scale);
  RS485_Reply(b);
}

/** Deterministic N-voice load: fixed freqs; equal scale so sum ≈ 1.0 FS. */
#define CPULOAD_BASE_HZ 220.0
#define CPULOAD_STEP_HZ 40.0

static void Console_CpuLoad_DisableVoices(void)
{
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    NoteBank_SetFreq(i, 0.0, 0.0);
  }
}

/** Enable the first `count` voices (1..16). Others off. */
static void Console_CpuLoad_EnableVoices(uint8_t count)
{
  double scale;

  if (count < 1u)
  {
    count = 1u;
  }
  if (count > NOTE_BANK_VOICES)
  {
    count = (uint8_t)NOTE_BANK_VOICES;
  }

  scale = 1.0 / (double)count;
  Console_CpuLoad_DisableVoices();
  for (uint8_t i = 0; i < count; i++)
  {
    NoteBank_SetFreq(i, CPULOAD_BASE_HZ + (CPULOAD_STEP_HZ * (double)i),
                     scale);
  }
}

/**
 * Parse cpuload mode + optional voice count.
 * Returns 1 and fills *mode_out / *nvoices on success; 0 on bad input.
 * *mode_out: 0=off, 1=dma/on, 2=queue.
 */
static uint8_t Console_CpuLoad_Parse(const char *arg, uint8_t *mode_out,
                                     uint8_t *nvoices_out)
{
  char mode_tok[12];
  unsigned int nvoices = NOTE_BANK_VOICES;
  unsigned int only_n;
  int nscan;

  *nvoices_out = (uint8_t)NOTE_BANK_VOICES;

  if (arg == NULL || *arg == '\0')
  {
    *mode_out = 1u; /* bare cpuload → on, 16 voices */
    return 1u;
  }

  /* "cpuload 4" → on with 4 voices (not "cpuload 1" as synonym for on). */
  if (sscanf(arg, "%u", &only_n) == 1)
  {
    char trail[8];
    if (sscanf(arg, "%u %7s", &only_n, trail) != 1)
    {
      return 0u; /* e.g. "4 junk" */
    }
    if (only_n == 0u)
    {
      *mode_out = 0u;
      *nvoices_out = 0u;
      return 1u;
    }
    if (only_n >= 1u && only_n <= NOTE_BANK_VOICES)
    {
      *mode_out = 1u;
      *nvoices_out = (uint8_t)only_n;
      return 1u;
    }
    return 0u;
  }

  nscan = sscanf(arg, "%11s %u", mode_tok, &nvoices);
  if (nscan < 1)
  {
    return 0u;
  }
  if (nscan == 2u)
  {
    if (nvoices < 1u || nvoices > NOTE_BANK_VOICES)
    {
      return 0u;
    }
    *nvoices_out = (uint8_t)nvoices;
  }

  if (strcmp(mode_tok, "off") == 0 || strcmp(mode_tok, "0") == 0)
  {
    *mode_out = 0u;
    return 1u;
  }
  if (strcmp(mode_tok, "on") == 0 || strcmp(mode_tok, "dma") == 0)
  {
    *mode_out = 1u;
    return 1u;
  }
  if (strcmp(mode_tok, "queue") == 0)
  {
    *mode_out = 2u;
    return 1u;
  }
  return 0u;
}

static void Console_Exec(char *line)
{
  char b[120];
  double hz;
  double scale;
  unsigned int ch, val;
  uint8_t note;
  int nscan;

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

  /* ---- cpuload [off|on|dma|queue] [1..16]: LED_Y busy/idle probe ---- */
  if (strncmp(line, "cpuload", 7) == 0 &&
      (line[7] == '\0' || line[7] == ' '))
  {
    const char *arg = line + 7;
    uint8_t mode;
    uint8_t nvoices;

    while (*arg == ' ')
    {
      arg++;
    }

    if (!Console_CpuLoad_Parse(arg, &mode, &nvoices))
    {
      RS485_Reply(
          "err: cpuload [off|on|dma|queue] [1..16]  "
          "(e.g. cpuload on 4, cpuload 1)\r\n");
      return;
    }

    if (mode == 0u)
    {
      Audio_CpuLoad_SetMode(AUDIO_CPULOAD_OFF);
      Console_CpuLoad_DisableVoices();
      led_show_on = 1;
      RS485_Reply("ok: cpuload off (notes cleared, LED chaser on)\r\n");
      return;
    }

    Console_ApplySessionDefaults();
    Audio_StartPlayback();
    Console_CpuLoad_EnableVoices(nvoices);
    led_show_on = 0;

    if (mode == 2u)
    {
      Audio_CpuLoad_SetMode(AUDIO_CPULOAD_QUEUE);
      snprintf(b, sizeof b,
               "ok: cpuload queue %u voices; scope yellow LED (PB9); "
               "low=busy high=idle\r\n",
               (unsigned)nvoices);
    }
    else
    {
      Audio_CpuLoad_SetMode(AUDIO_CPULOAD_DMA);
      snprintf(b, sizeof b,
               "ok: cpuload on %u voices (220+40*i Hz); scope yellow LED "
               "(PB9); low=busy; CPU%%≈t_low/(t_low+t_high)\r\n",
               (unsigned)nvoices);
    }
    RS485_Reply(b);
    return;
  }

  /* ---- n0..nf: 16-voice note bank on CH1 ---- */
  if (line[0] != 'n' || line[1] == '\0')
  {
    RS485_Reply(
        "err: commands are n0..nf <Hz> [scale] | gain <ch> <dB> | "
        "cpuload [off|on|dma|queue] [1..16]\r\n");
    return;
  }

  note = Console_ParseNoteSlot(line[1]);
  if (note == 0xFFu || (line[2] != '\0' && line[2] != ' '))
  {
    RS485_Reply(
        "err: commands are n0..nf <Hz> [scale] | gain <ch> <dB> | "
        "cpuload [off|on|dma|queue] [1..16]\r\n");
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

  nscan = sscanf(line + 3, "%lf %lf", &hz, &scale);
  if (nscan == 1)
  {
    scale = 1.0; /* default max amplitude */
  }
  else if (nscan != 2)
  {
    snprintf(b, sizeof b,
             "err: n%c <Hz> [scale] (0=off, else 20..19999.9, scale 0..1)\r\n",
             line[1]);
    RS485_Reply(b);
    return;
  }

  Console_SetNoteFreq(note, hz, scale);
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
  static uint32_t dropped_reported = 0;
  uint8_t c;

  /* Fail loud on lost characters — but only when idle between lines.
   * Replying mid-command would itself drive the bus and lose more RX. */
  const uint32_t dropped = Uart5Rx_DroppedCount();
  if (dropped != dropped_reported && idx == 0u)
  {
    char b[64];
    snprintf(b, sizeof b, "err: rs485 rx dropped %lu bytes\r\n",
             (unsigned long)dropped);
    dropped_reported = dropped;
    RS485_Reply(b);
  }

  while (Uart5Rx_Get(&c))
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
      /* Multi-drop: Effect echoes every host keystroke onto the same wire.
       * Our IRQ RX now catches that echo mixed into the real frame, which
       * showed up as "cjc:n0…" (Channel then failed to parse). Whenever a
       * fresh "c:"/"e:"/"*:" address lands, discard everything before it. */
      if (idx >= 2u && cmd[idx - 1u] == ':' &&
          (cmd[idx - 2u] == RS485_CARD_ID ||
           cmd[idx - 2u] == RS485_BROADCAST_ID || cmd[idx - 2u] == 'e'))
      {
        cmd[0] = cmd[idx - 2u];
        cmd[1] = ':';
        idx = 2u;
      }
#if RS485_ECHO
      char e[2] = {(char)c, '\0'};
      RS485_Send(e); /* echo typing */
#endif
    }
  }
}

/** Non-blocking 5-LED chaser (150 ms per step) so the console stays snappy.
 * Skipped while cpuload probe owns LED_Y. */
static void LED_Task(void)
{
  static const GPIO_TypeDef *ports[5] = {LED_R_GPIO_Port, LED_Y_GPIO_Port,
                                         RGB_R_GPIO_Port, RGB_G_GPIO_Port,
                                         RGB_B_GPIO_Port};
  static const uint16_t pins[5] = {LED_R_Pin, LED_Y_Pin, RGB_R_Pin, RGB_G_Pin,
                                   RGB_B_Pin};
  static uint32_t t_next = 0;
  static uint8_t step = 0;

  if (Audio_CpuLoad_IsActive() || !led_show_on)
  {
    if (!Audio_CpuLoad_IsActive())
    {
      for (uint8_t i = 0; i < 5; i++)
      {
        HAL_GPIO_WritePin((GPIO_TypeDef *)ports[i], pins[i], GPIO_PIN_RESET);
      }
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


void ChannelConsole_Init(void)
{
  RS485_BusRelease();
  Uart5Rx_Init();

  for (uint8_t i = 0; i < NUM_SWITCHES; i++)
  {
    HAL_GPIO_WritePin(switches[i].port, switches[i].pin,
                      switches[i].active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  Console_ApplySessionDefaults();

  RS485_Reply("\r\n"
              "**************************************************\r\n"
              "*  Channel Card [C] ready  (multi-drop RS485)    *\r\n"
              "*  n0..nf <Hz> [scale] | gain <ch> <dB>          *\r\n"
              "*  cpuload [on|off] [1..16]  — yellow LED PB9   *\r\n"
              "*  enter/boot: bypass ON, gain 1 0               *\r\n"
              "**************************************************\r\n");
}

void ChannelConsole_Poll(void)
{
  Console_Poll();
  LED_Task();
}
