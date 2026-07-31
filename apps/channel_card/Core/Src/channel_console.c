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
/** I2S2 self-heal restarts — nonzero means the audio path is erroring. */
extern volatile uint32_t g_i2s2_err_restarts;

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
/** When set, the card never drives RS485 for console replies. MIDI note
 * bursts collide with any TX (ok:, err:, drop reports) and garble Offs into
 * stuck voices — quiet must mute the bus completely, not just ok:. */
static uint8_t rs485_quiet = 0;

/* RS485 path counters — read back with "diag" (works over USB CDC while the
 * bus itself is unresponsive, which is the only way to see these). */
static uint32_t rs485_cmd_count;  /**< commands executed from the bus */
static uint32_t rs485_tx_fail;    /**< HAL_UART_Transmit did not return OK */
static uint32_t rs485_tx_trunc;   /**< reply longer than the frame buffer */

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied. No-op on RS485 while quiet (USB CDC still replies).
 *
 * Frame must hold the longest reply (cpuload/status are ~110 chars) — a
 * short buffer silently swallowed those ACKs and looked like a dead bus.
 * Truncate rather than drop: a clipped line still terminates the host's
 * exchange, silence does not. */
static void RS485_Reply(const char *s)
{
  char frame[160];
  size_t tag_len;
  size_t body_len;
  size_t max_body;

  if (console_via_usb)
  {
    USB_CDC_WriteStr(s);
    return;
  }
  if (rs485_quiet)
  {
    return;
  }

  tag_len = strlen(RS485_TAG);
  body_len = strlen(s);
  max_body = sizeof(frame) - tag_len - 1u;
  if (body_len > max_body)
  {
    body_len = max_body;
    rs485_tx_trunc++;
  }
  memcpy(frame, RS485_TAG, tag_len);
  memcpy(frame + tag_len, s, body_len);

  RS485_BusAcquire();
  if (HAL_UART_Transmit(&huart5, (const uint8_t *)frame,
                        (uint16_t)(tag_len + body_len), 50) != HAL_OK)
  {
    rs485_tx_fail++;
  }
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
 *   n0..nf <Hz> [sc]  — note freq; optional scale 0.0..1.0 (default 0.125)
 *   gain <ch> <dB>    — CS4304 per-channel DAC atten (0..127 dB), ch 1..4
 *   cpuload off|on|dma|queue — LED_Y (PB9) CPU-load probe (see README) */
#define N0_DEFAULT_ATTEN_DB 0u

/** Bypass is active-low (LOW = ON). */
static void Console_SetBypassOn(void)
{
  HAL_GPIO_WritePin(BYPASS_SW_GPIO_Port, BYPASS_SW_Pin, GPIO_PIN_RESET);
}

/** Defaults for bare n0 / boot: dry path + CH1 DAC trim 0 dB (`gain 1 0`).
 * Frequency changes (`nX <Hz>`) do not touch gain or bypass.
 * Also clears rs485_quiet so a host that died mid-session (Ctrl+C) can
 * still get an n0 reply on the next open. */
static void Console_ApplySessionDefaults(void)
{
  rs485_quiet = 0;
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

/** Production MIDI scale when the host omits [scale] (byte-minimal nX Hz). */
#define NOTE_DEFAULT_SCALE 0.125

/** Apply nX <Hz> [scale]. Compact ACK: ok / err:<code>. */
static void Console_SetNoteFreq(uint8_t note, double hz, double scale)
{
  if (hz <= 0.0)
  {
    NoteBank_SetFreq(note, 0.0, 0.0);
    RS485_Reply("ok\r\n");
    return;
  }

  if (hz < 20.0 || hz >= 20000.0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }

  if (scale < 0.0 || scale > 1.0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }

  NoteBank_SetFreq(note, hz, scale);
  RS485_Reply("ok\r\n");
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
      RS485_Reply("ok\r\n");
    }
    else
    {
      RS485_Reply("err:range\r\n");
    }
    return;
  }

  /* ---- quiet on|off: mute ALL RS485 replies (lab / legacy; not for MIDI) ---- */
  if (strcmp(line, "quiet on") == 0)
  {
    /* Reply before arming quiet — RS485_Reply is a no-op once set. */
    RS485_Reply("ok\r\n");
    rs485_quiet = 1;
    return;
  }
  if (strcmp(line, "quiet off") == 0)
  {
    rs485_quiet = 0;
    RS485_Reply("ok\r\n");
    return;
  }
  if (strcmp(line, "quiet") == 0)
  {
    snprintf(b, sizeof b, "quiet: %s  (usage: quiet on|off)\r\n",
             rs485_quiet ? "on" : "off");
    RS485_Reply(b);
    return;
  }

  /* ---- diag: RS485 path counters (read over USB CDC when bus is dead) ---- */
  if (strcmp(line, "diag") == 0)
  {
    snprintf(b, sizeof b,
             "ok: rxdrop=%lu cmd=%lu txfail=%lu trunc=%lu i2serr=%lu\r\n",
             (unsigned long)Uart5Rx_DroppedCount(),
             (unsigned long)rs485_cmd_count, (unsigned long)rs485_tx_fail,
             (unsigned long)rs485_tx_trunc,
             (unsigned long)g_i2s2_err_restarts);
    RS485_Reply(b);
    return;
  }

  /* ---- silence: turn off every note-bank voice ---- */
  if (strcmp(line, "silence") == 0)
  {
    Console_CpuLoad_DisableVoices();
    RS485_Reply("ok\r\n");
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
      RS485_Reply("err:syntax\r\n");
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
    RS485_Reply("err:unknown\r\n");
    return;
  }

  note = Console_ParseNoteSlot(line[1]);
  if (note == 0xFFu || (line[2] != '\0' && line[2] != ' '))
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }

  /* Bare "n0" = session defaults only. Bare n1..nf are not session cmds. */
  if (line[2] == '\0')
  {
    if (note == 0u)
    {
      Console_ApplySessionDefaults();
      RS485_Reply("ok\r\n");
    }
    else
    {
      RS485_Reply("err:syntax\r\n");
    }
    return;
  }

  nscan = sscanf(line + 3, "%lf %lf", &hz, &scale);
  if (nscan == 1)
  {
    scale = NOTE_DEFAULT_SCALE; /* production MIDI default */
  }
  else if (nscan != 2)
  {
    RS485_Reply("err:syntax\r\n");
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

/** MIDI host binary bank: magic 0x01 + 16×uint16 LE Hz + sum8.
 * Fixed scale 0.125 (matches midi_host). Reject whole frame on bad sum/Hz. */
#define BINBANK_MAGIC 0x01u
#define BINBANK_HZ_BYTES 32u
#define BINBANK_SCALE 0.125
/** Wire bytes before CR: "c:" + magic + 32 Hz + sum8. */
#define BINBANK_FRAME_LEN 36u

static void Console_ApplyBinBank(const uint8_t *hz32, uint8_t sum8)
{
  uint8_t sum = 0;
  uint16_t hz[NOTE_BANK_VOICES];

  for (uint8_t i = 0; i < BINBANK_HZ_BYTES; i++)
  {
    sum = (uint8_t)(sum + hz32[i]);
  }
  if (sum != sum8)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }

  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    const uint16_t v =
        (uint16_t)hz32[(uint8_t)(2u * i)] |
        ((uint16_t)hz32[(uint8_t)(2u * i + 1u)] << 8);
    if (v != 0u && (v < 20u || v >= 20000u))
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    hz[i] = v;
  }

  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    if (hz[i] == 0u)
    {
      NoteBank_SetFreq(i, 0.0, 0.0);
    }
    else
    {
      NoteBank_SetFreq(i, (double)hz[i], BINBANK_SCALE);
    }
  }
  RS485_Reply("ok\r\n"); /* no-op when quiet */
}

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * Messages addressed to other cards are silently dropped.
 * After "c:"/"*:", magic 0x01 switches to raw binary bank mode. */
static void Console_Poll(void)
{
  static uint8_t cmd[40];
  static uint8_t idx = 0;
  static uint8_t bin_mode = 0;
  static uint32_t dropped_reported = 0;
  uint8_t c;

  /* Fail loud on lost characters — but only when idle between lines.
   * Replying mid-command would itself drive the bus and lose more RX. */
  const uint32_t dropped = Uart5Rx_DroppedCount();
  if (dropped != dropped_reported && idx == 0u)
  {
    dropped_reported = dropped;
    RS485_Reply("err:rxdrop\r\n");
  }

  while (Uart5Rx_Get(&c))
  {
    /* Binary bank payload is length-framed: Hz bytes / sum8 may be 0x0D/0x0A.
     * Only treat CR/LF as the terminator after the fixed 36-byte header. */
    if (bin_mode && idx < BINBANK_FRAME_LEN)
    {
      cmd[idx++] = c;
      continue;
    }

    if (c == '\r' || c == '\n')
    {
#if RS485_ECHO
      RS485_Send("\r\n");
#endif
      if (bin_mode)
      {
        /* Exact length: c: / *: + magic + 32 Hz + sum8. */
        if (idx == BINBANK_FRAME_LEN && cmd[1] == ':' &&
            (cmd[0] == (uint8_t)RS485_CARD_ID ||
             cmd[0] == (uint8_t)RS485_BROADCAST_ID) &&
            cmd[2] == BINBANK_MAGIC)
        {
          /* Legacy binary bank — deprecated; prefer ASCII nX one-by-one. */
          Console_ApplyBinBank(&cmd[3], cmd[35]);
        }
        else if (cmd[0] == (uint8_t)RS485_CARD_ID ||
                 cmd[0] == (uint8_t)RS485_BROADCAST_ID)
        {
          RS485_Reply("err:syntax\r\n");
        }
        bin_mode = 0;
        idx = 0;
        continue;
      }

      cmd[idx] = '\0';

      /* Card-address filtering: only execute if addressed to us.
       * No artificial post-Enter delay — production requires e:echo off. */
      if (RS485_IsForMe((char *)cmd))
      {
        rs485_cmd_count++;
        Console_Exec((char *)cmd);
      }
      /* else: message for another card — silently ignore */

      idx = 0;
    }
    else if (bin_mode)
    {
      /* idx == BINBANK_FRAME_LEN already; non-CR junk → abandon. */
      bin_mode = 0;
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
    else if (c == BINBANK_MAGIC && idx == 2u && cmd[1] == ':' &&
             (cmd[0] == (uint8_t)RS485_CARD_ID ||
              cmd[0] == (uint8_t)RS485_BROADCAST_ID))
    {
      /* "c:" / "*:" then magic → raw bank payload (any byte values). */
      cmd[idx++] = c;
      bin_mode = 1;
    }
    else if (c >= 32 && c < 127 && idx < sizeof(cmd) - 1)
    {
      cmd[idx++] = (uint8_t)((c >= 'A' && c <= 'Z') ? c + 32 : c);
      /* Multi-drop: Effect echoes every host keystroke onto the same wire.
       * Our IRQ RX now catches that echo mixed into the real frame, which
       * showed up as "cjc:n0…" (Channel then failed to parse). Whenever a
       * fresh "c:"/"e:"/"*:" address lands, discard everything before it. */
      if (idx >= 2u && cmd[idx - 1u] == ':' &&
          (cmd[idx - 2u] == (uint8_t)RS485_CARD_ID ||
           cmd[idx - 2u] == (uint8_t)RS485_BROADCAST_ID ||
           cmd[idx - 2u] == (uint8_t)'e'))
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
              "*  silence | quiet on|off | cpuload ...          *\r\n"
              "*  replies: [C]ok / [C]err:<code>  (CRLF)        *\r\n"
              "*  enter/boot: bypass ON, gain 1 0               *\r\n"
              "**************************************************\r\n");
}

void ChannelConsole_Poll(void)
{
  Console_Poll();
  LED_Task();
}
