/**
 ******************************************************************************
 * @file    channel_console.c
 * @brief   Channel Card RS485 + USB CDC console (short cmds), cpu probe, LED.
 *
 * main.c owns bring-up and the DAC handle; this file owns the interactive
 * console surface.
 ******************************************************************************
 */

#include "channel_console.h"

#include "main.h"
#include "usart.h"
#include "cs4304.h"
#include "channel_led.h"
#include "audio_bridge.h"
#include "note_bank.h"
#include "note_envelope.h"
#include "note_filter.h"
#include "uart5_rx.h"
#include "usb_app.h"
#include "usb_stream.h"
#include "attack_bank.h"
#include "attack_upload.h"
#include "vm_upload.h"
#include "freshwater/vm_channel.h"
#include "stream_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern UART_HandleTypeDef huart5;

/* Bound from main via ChannelConsole_SetDacHandle(). */
static CS4304_HandleTypeDef *s_dac;

void ChannelConsole_SetDacHandle(CS4304_HandleTypeDef *h)
{
  s_dac = h;
}

/* ---- Multi-drop RS485 card addressing ---- */
#define RS485_CARD_ID 'c'
#define RS485_BROADCAST_ID '*'
#define RS485_BUS_TIMEOUT_MS 250
#define RS485_ECHO 0

/* 921600 8N1 ≈ 11 µs/byte. HAL Timeout is a deadline, not the payload
 * size. The 56-byte ABI6 vq frame is bounded; 50 ms was a poll
 * cap that could eat TinyUSB's ~32 ms ISO software FIFO if TX stalled. */
static uint32_t RS485_TxDeadlineMs(uint32_t nbytes)
{
  uint32_t ms = 2u + (nbytes / 64u);
  if (ms > 8u)
  {
    ms = 8u;
  }
  return ms;
}

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
  gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gi.Alternate = GPIO_AF8_UART5;
  HAL_GPIO_Init(GPIOC, &gi);

  HAL_GPIO_WritePin(RS485_CTL_GPIO_Port, RS485_CTL_Pin, GPIO_PIN_SET);
  gi.Pin = RS485_CTL_Pin;
  gi.Mode = GPIO_MODE_OUTPUT_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
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

  {
    uint32_t n = (uint32_t)strlen(s);
    HAL_UART_Transmit(&huart5, (const uint8_t *)s, (uint16_t)n,
                      RS485_TxDeadlineMs(n));
  }

  RS485_BusRelease();
  return 0;
}

/* When a command arrives over the USB CDC console, replies must go back
 * over USB instead of the RS485 bus.  Console_ExecFromUSB() sets this
 * for the duration of the command. */
static uint8_t console_via_usb = 0;

/* RS485 path counters (internal; no console cmd). */
static uint32_t rs485_cmd_count; /**< commands executed from the bus */
static uint32_t rs485_tx_fail;   /**< HAL_UART_Transmit did not return OK */
static uint32_t rs485_tx_trunc;  /**< reply longer than the frame buffer */
static uint32_t rs485_vq_count;  /**< exact refill-status frames transmitted */

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied.
 *
 * Frame must hold the longest reply (~110 chars) — a short buffer silently
 * swallowed those ACKs and looked like a dead bus. Truncate rather than
 * drop: a clipped line still terminates the host's exchange. */
static void RS485_Reply(const char *s)
{
  char frame[224];
  size_t tag_len;
  size_t body_len;
  size_t max_body;

  if (console_via_usb)
  {
    USB_CDC_WriteStr(s);
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
                        (uint16_t)(tag_len + body_len),
                        RS485_TxDeadlineMs((uint32_t)(tag_len + body_len))) !=
      HAL_OK)
  {
    rs485_tx_fail++;
  }
  RS485_BusRelease();
}

/*
 * Sequenced vq frame: masks/capacity/status and last processed UAC sequence,
 * plus eight target records, CRC-8 and terminator.
 */
#define VQ_FRAME_LEN 56u
#define VQ_SYNC_0 0xA5u
#define VQ_SYNC_1 0x5Au
#define VQ_CARD_CHANNEL 0x43u
#define VQ_TYPE_STATUS 0x04u
_Static_assert(NOTE_BANK_VOICES == 8u,
               "vq binary frame packs exactly eight voices");
_Static_assert(STREAM_RING_SAMPLES <= 65535u,
               "vq exact free count is uint16");

static uint8_t RS485_Crc8(const uint8_t *data, uint32_t len)
{
  uint8_t crc = 0u;
  uint32_t i;
  uint8_t bit;

  for (i = 0u; i < len; i++)
  {
    crc ^= data[i];
    for (bit = 0u; bit < 8u; bit++)
    {
      crc = (crc & 0x80u) != 0u ? (uint8_t)((crc << 1u) ^ 0x07u)
                                : (uint8_t)(crc << 1u);
    }
  }
  return crc;
}

static uint16_t rs485_vq_sequence;
static void RS485_ReplyVq(uint8_t active_mask, uint8_t pending_mask,
                          uint8_t best, const uint8_t *sessions,
                          const uint16_t *fills, const uint16_t *free_samples)
{
  uint8_t frame[VQ_FRAME_LEN];
  uint8_t i;

  frame[0] = VQ_SYNC_0;
  frame[1] = VQ_SYNC_1;
  frame[2] = VQ_CARD_CHANNEL;
  frame[3] = VQ_TYPE_STATUS;
  frame[4] = active_mask;
  frame[5] = pending_mask;
  frame[6] = best;
  frame[7] = 0u;
  frame[8] = (uint8_t)(STREAM_RING_SAMPLES & 0xFFu);
  frame[9] = (uint8_t)(STREAM_RING_SAMPLES >> 8u);
  rs485_vq_sequence++;
  frame[10] = (uint8_t)(rs485_vq_sequence & 0xFFu);
  frame[11] = (uint8_t)(rs485_vq_sequence >> 8u);
  {
    const uint16_t uac_sequence = StreamRing_LastUacSequence();
    frame[12] = (uint8_t)(uac_sequence & 0xFFu);
    frame[13] = (uint8_t)(uac_sequence >> 8u);
  }
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    uint8_t *record = frame + 14u + (5u * i);
    record[0] = sessions[i];
    record[1] = (uint8_t)(fills[i] & 0xFFu);
    record[2] = (uint8_t)(fills[i] >> 8u);
    record[3] = (uint8_t)(free_samples[i] & 0xFFu);
    record[4] = (uint8_t)(free_samples[i] >> 8u);
  }
  frame[54] = RS485_Crc8(frame, 54u);
  frame[55] = '\n';

  RS485_BusAcquire();
  if (HAL_UART_Transmit(&huart5, frame, VQ_FRAME_LEN,
                        RS485_TxDeadlineMs(VQ_FRAME_LEN)) != HAL_OK)
  {
    rs485_tx_fail++;
  }
  else
  {
    rs485_vq_count++;
  }
  RS485_BusRelease();
}

/* ---------------- Channel Card control console (RS485) ---------------- */

static uint8_t led_show_on = 1;

/* Analog-switch GPIO table. Driven once at init (all OFF, then the
 * session default turns bypass ON); there is no runtime switch command. */
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

/* Console commands (RS485 + USB CDC). Console_Poll lowercases.
 *
 *   h / help / ?      — command list
 *   n0                — session defaults: bypass ON + g 1 0
 *   n0..n7 on <key> [@session] / off — raw MIDI-key gate
 *   n off             — release all 8 voices
 *   s / p <d> / t <a> — global note-bank shape (sine / pulse duty / tri asym)
 *   al <id> <len>     — CDC attack-head upload (2..ATTACK_BANK_BYTES)
 *   vmload <v> <len>  — CDC Berry ABI6 upload; vm [v|mem] — status
 *   ar <id> <Hz>      — sample root pitch (id = wave 0..255); a — loaded mask
 *   a / vq            — loaded heads per voice / hungriest + exact credit
 *   usb               — BODY counters: drop/hold/min/fill/z/sof/rx/bytes/bad
 *   usb 0             — clear those counters, then same reply
 *   f0..f7 <Hz> [q] / f <Hz> [q] — LPF on the 8 voices (0 or 20000 = bypass;
 *                        q = DF4 g 0.5..10, default 1.0; higher = more peak)
 *   fk0..fk7 / fk     — filter pitch-track k (0..10, fc = fbase*(f/C4)^k)
 *   g <ch> <dB>       — CS4304 DAC atten dB (0..127; 0.5 dB reg steps), ch 1..4
 *   cpu [0|N|q [N]]   — LED_Y load probe (see README) */
#define N0_DEFAULT_ATTEN_DB 0u
/** Console-addressable filter slots (voices 8..15 stay boot bypass). */
#define NOTE_FILTER_CONSOLE_VOICES 8u

/** Bypass is active-low (LOW = ON). */
static void Console_SetBypassOn(void)
{
  HAL_GPIO_WritePin(BYPASS_SW_GPIO_Port, BYPASS_SW_Pin, GPIO_PIN_RESET);
}

/** Defaults for bare n0 / boot: dry path + CH1 DAC trim 0 dB (`g 1 0`).
 * Key changes (`nX on <key>`) do not touch gain or bypass. */
static void Console_ApplySessionDefaults(void)
{
  Console_SetBypassOn();
  if (s_dac != NULL)
  {
    CS4304_SetChannelTrim(s_dac, '1', (uint8_t)(N0_DEFAULT_ATTEN_DB * 2u));
  }
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

/** Apply nX on <key> [@session]. Compact ACK: ok / err:<code>. */
static void Console_NoteOn(uint8_t note, uint8_t key, uint16_t session)
{
  if (NoteBank_VmUploadIsBusy() != 0u)
  {
    RS485_Reply("err:vm-busy\r\n");
    return;
  }
  if (NoteBank_VmIsActive(note) == 0u)
  {
    RS485_Reply("err:no-program\r\n");
    return;
  }

  if (session < USB_STREAM_SESSION_MOD)
  {
    NoteBank_NoteOnSession(note, key, (uint8_t)session);
  }
  else
  {
    NoteBank_NoteOn(note, key);
  }
  RS485_Reply("ok\r\n");
}

/** Deterministic N-voice load: fixed freqs; equal scale so sum ≈ 1.0 FS. */
static void Console_CpuLoad_DisableVoices(void)
{
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    NoteBank_NoteOff(i);
  }
}

/** Enable the first `count` voices (1..16). Others off. */
static void Console_CpuLoad_EnableVoices(uint8_t count)
{
  if (count < 1u)
  {
    count = 1u;
  }
  if (count > NOTE_BANK_VOICES)
  {
    count = (uint8_t)NOTE_BANK_VOICES;
  }

  Console_CpuLoad_DisableVoices();
  for (uint8_t i = 0; i < count; i++)
  {
    NoteBank_NoteOn(i, (uint8_t)(57u + i));
  }
}

/**
 * Parse cpu mode + optional voice count.
 * Returns 1 and fills *mode_out / *nvoices on success; 0 on bad input.
 * *mode_out: 0=off, 1=dma/on, 2=queue.
 * Args: bare | 0 | N | q | q N  (no on/off/dma/queue words).
 */
static uint8_t Console_CpuLoad_Parse(const char *arg, uint8_t *mode_out,
                                     uint8_t *nvoices_out)
{
  char mode_tok[8];
  unsigned int nvoices = NOTE_BANK_VOICES;
  unsigned int only_n;
  int nscan;

  *nvoices_out = (uint8_t)NOTE_BANK_VOICES;

  if (arg == NULL || *arg == '\0')
  {
    *mode_out = 1u; /* bare cpu → on, all NOTE_BANK_VOICES voices */
    return 1u;
  }

  /* "cpu 0" / "cpu 8" */
  if (sscanf(arg, "%u", &only_n) == 1)
  {
    char trail[8];
    if (sscanf(arg, "%u %7s", &only_n, trail) != 1)
    {
      return 0u;
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

  /* "cpu q" / "cpu q 8" */
  nscan = sscanf(arg, "%7s %u", mode_tok, &nvoices);
  if (nscan < 1 || strcmp(mode_tok, "q") != 0)
  {
    return 0u;
  }
  if (nscan == 2)
  {
    if (nvoices < 1u || nvoices > NOTE_BANK_VOICES)
    {
      return 0u;
    }
    *nvoices_out = (uint8_t)nvoices;
  }
  *mode_out = 2u;
  return 1u;
}

static void Console_Help(void)
{
  char b[256];
  /* One tagged line — leading \\r\\n would make the host see bare "[C]". */
  snprintf(b, sizeof b,
           "ok: SAMPLE n0..n7 on key [@session] | off | s|p d|t a|saw | "
           "aw v id | "
           "al id n | vmload v n | vm [v] | ar id Hz | a | vq | "
           "usb | "
           "f0..f7 Hz [q] | fk0..fk7 k | g ch dB | "
           "cpu [0|N|q N]\r\n");
  RS485_Reply(b);
}

static void Console_ShapeReply(void)
{
  char b[40];
  NoteBank_Shape_t sh = NoteBank_GetShape();

  if (sh == NOTE_SHAPE_PULSE)
  {
    snprintf(b, sizeof b, "ok: p %.2f\r\n", NoteBank_GetShapeParam());
  }
  else if (sh == NOTE_SHAPE_TRI)
  {
    snprintf(b, sizeof b, "ok: t %.2f\r\n", NoteBank_GetShapeParam());
  }
  else if (sh == NOTE_SHAPE_SAW)
  {
    snprintf(b, sizeof b, "ok: saw\r\n");
  }
  else
  {
    snprintf(b, sizeof b, "ok: s\r\n");
  }
  RS485_Reply(b);
}

static void Console_FilterReply(uint8_t voice)
{
  char b[128];
  double base = NoteFilter_GetBaseCutoff(voice);
  double fc = NoteFilter_GetCutoff(voice);
  double k = NoteFilter_GetPitchK(voice);

  if (fc >= NOTE_FILTER_CUTOFF_MAX_HZ)
  {
    snprintf(b, sizeof b, "ok: f%u %.1f Hz (bypass) k %.2f q %.2f\r\n",
             (unsigned)voice, base, k, NoteFilter_GetQ(voice));
  }
  else if (k > 0.0)
  {
    snprintf(b, sizeof b, "ok: f%u base %.1f → %.1f Hz k %.2f q %.2f\r\n",
             (unsigned)voice, base, fc, k, NoteFilter_GetQ(voice));
  }
  else
  {
    snprintf(b, sizeof b, "ok: f%u %.1f Hz k %.2f q %.2f\r\n", (unsigned)voice,
             fc, k, NoteFilter_GetQ(voice));
  }
  RS485_Reply(b);
}

static void Console_FkReply(uint8_t voice)
{
  char b[48];
  snprintf(b, sizeof b, "ok: fk%u %.2f\r\n", (unsigned)voice,
           NoteFilter_GetPitchK(voice));
  RS485_Reply(b);
}

/** g <ch> <dB>: CS4304 DAC atten. Caller already matched sscanf. */
static void Console_CmdGain(char *line)
{
  unsigned int ch, val;

  (void)sscanf(line, "g %u %u", &ch, &val);
  if (ch >= 1 && ch <= 4 && val <= 127 && s_dac != NULL)
  {
    CS4304_SetChannelTrim(s_dac, (char)('0' + ch), (uint8_t)(val * 2u));
    RS485_Reply("ok\r\n");
  }
  else
  {
    RS485_Reply("err:range\r\n");
  }
}

/** f0..f7 / f: LPF on first 8 voices; optional q = DF4 g. */
static void Console_CmdFilter(char *line, char *b, size_t bsz)
{
  const char *rest;
  double fc;
  double q;
  int rc;
  int nscan;
  uint8_t note;

  if (line[1] >= '0' && line[1] <= '7' &&
      (line[2] == '\0' || line[2] == ' '))
  {
    note = (uint8_t)(line[1] - '0');
    rest = line + 2;
    while (*rest == ' ')
    {
      rest++;
    }
    if (*rest == '\0')
    {
      Console_FilterReply(note);
      return;
    }
    nscan = sscanf(rest, "%lf %lf", &fc, &q);
    if (nscan < 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    if (nscan == 2)
    {
      rc = NoteFilter_SetQ(note, q);
      if (rc != 0)
      {
        RS485_Reply("err:range\r\n");
        return;
      }
    }
    rc = NoteFilter_SetCutoff(note, fc);
    if (rc != 0)
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    Console_FilterReply(note);
    return;
  }

  if ((line[1] >= '8' && line[1] <= '9') ||
      (line[1] >= 'a' && line[1] <= 'f'))
  {
    if (line[2] == '\0' || line[2] == ' ')
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    RS485_Reply("err:syntax\r\n");
    return;
  }

  if (line[1] == '\0' || line[1] == ' ')
  {
    rest = line + 1;
    while (*rest == ' ')
    {
      rest++;
    }
    if (*rest == '\0')
    {
      /* Compact dump of f0..f7 effective/q/k. */
      int n = snprintf(b, bsz, "ok:");
      for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES && n > 0 &&
                          (size_t)n < bsz;
           i++)
      {
        fc = NoteFilter_GetCutoff(i);
        n += snprintf(b + n, bsz - (size_t)n, " f%u=%.0f/%.2f/k%.2f",
                      (unsigned)i, fc, NoteFilter_GetQ(i),
                      NoteFilter_GetPitchK(i));
      }
      if ((size_t)n < bsz - 2u)
      {
        snprintf(b + n, bsz - (size_t)n, "\r\n");
      }
      RS485_Reply(b);
      return;
    }
    nscan = sscanf(rest, "%lf %lf", &fc, &q);
    if (nscan < 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++)
    {
      if (nscan == 2)
      {
        rc = NoteFilter_SetQ(i, q);
        if (rc != 0)
        {
          RS485_Reply("err:range\r\n");
          return;
        }
      }
      rc = NoteFilter_SetCutoff(i, fc);
      if (rc != 0)
      {
        RS485_Reply("err:range\r\n");
        return;
      }
    }
    snprintf(b, bsz, "ok: f %.1f Hz%s k %.2f q %.2f\r\n", fc,
             (fc == 0.0 || fc >= NOTE_FILTER_CUTOFF_MAX_HZ) ? " (bypass)" : "",
             NoteFilter_GetPitchK(0), NoteFilter_GetQ(0));
    RS485_Reply(b);
    return;
  }

  RS485_Reply("err:unknown\r\n");
}

/** fk / fk0..fk7: filter pitch-track k (voices 0..7). */
static void Console_CmdFk(char *line, char *b, size_t bsz)
{
  const char *rest;
  double kd;
  int rc;
  uint8_t note;

  if (line[2] == '\0' || line[2] == ' ')
  {
    rest = line + 2;
    while (*rest == ' ')
    {
      rest++;
    }
    if (*rest == '\0')
    {
      int n = snprintf(b, bsz, "ok:");
      for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES && n > 0 &&
                          (size_t)n < bsz;
           i++)
      {
        n += snprintf(b + n, bsz - (size_t)n, " fk%u=%.2f", (unsigned)i,
                      NoteFilter_GetPitchK(i));
      }
      if ((size_t)n < bsz - 2u)
      {
        snprintf(b + n, bsz - (size_t)n, "\r\n");
      }
      RS485_Reply(b);
      return;
    }
    if (sscanf(rest, "%lf", &kd) != 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++)
    {
      rc = NoteFilter_SetPitchK(i, kd);
      if (rc != 0)
      {
        RS485_Reply("err:range\r\n");
        return;
      }
    }
    snprintf(b, bsz, "ok: fk %.2f\r\n", kd);
    RS485_Reply(b);
    return;
  }

  if (line[2] >= '0' && line[2] <= '7' &&
      (line[3] == '\0' || line[3] == ' '))
  {
    note = (uint8_t)(line[2] - '0');
    rest = line + 3;
    while (*rest == ' ')
    {
      rest++;
    }
    if (*rest == '\0')
    {
      Console_FkReply(note);
      return;
    }
    if (sscanf(rest, "%lf", &kd) != 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    rc = NoteFilter_SetPitchK(note, kd);
    if (rc != 0)
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    Console_FkReply(note);
    return;
  }

  if ((line[2] >= '8' && line[2] <= '9') ||
      (line[2] >= 'a' && line[2] <= 'f'))
  {
    if (line[3] == '\0' || line[3] == ' ')
    {
      RS485_Reply("err:range\r\n");
      return;
    }
  }
  RS485_Reply("err:syntax\r\n");
}

/** cpu [0|N|q [N]]: LED_Y busy/idle probe. */
static void Console_CmdCpu(char *line, char *b, size_t bsz)
{
  const char *arg = line + 3;
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
    RS485_Reply("ok: cpu 0\r\n");
    return;
  }

  if (NoteBank_VmUploadIsBusy() != 0u)
  {
    RS485_Reply("err:vm-busy\r\n");
    return;
  }

  Console_ApplySessionDefaults();
  Audio_StartPlayback();
  Console_CpuLoad_EnableVoices(nvoices);
  led_show_on = 0;

  if (mode == 2u)
  {
    Audio_CpuLoad_SetMode(AUDIO_CPULOAD_QUEUE);
    snprintf(b, bsz, "ok: cpu q %u\r\n", (unsigned)nvoices);
  }
  else
  {
    Audio_CpuLoad_SetMode(AUDIO_CPULOAD_DMA);
    snprintf(b, bsz, "ok: cpu %u\r\n", (unsigned)nvoices);
  }
  RS485_Reply(b);
}

/**
 * vq — ABI6 target identity/fill plus exact total writable credit.
 */
static void Console_CmdVoiceQuery(void)
{
  char b[256];
  int n;
  uint8_t mask = 0u;
  uint8_t best = 0xFFu;
  uint16_t free_samples[NOTE_BANK_VOICES];
  uint8_t pending_mask = 0u;
  uint8_t sessions[NOTE_BANK_VOICES];
  uint16_t fills[NOTE_BANK_VOICES];
  uint8_t i;

  NoteBank_VoiceQuery(&mask, &best);
  {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    free_samples[i] = (uint16_t)StreamRing_FreeLevel(i);
    sessions[i] = StreamRing_TargetSession(i);
    fills[i] = (uint16_t)StreamRing_TargetFill(i);
    if (StreamRing_HasPending(i) != 0u)
      pending_mask = (uint8_t)(pending_mask | (uint8_t)(1u << i));
  }
    if (primask == 0u) __enable_irq();
  }

  n = snprintf(b, sizeof b, "ok:vq7 %02x %02x %u %u %u %u",
               (unsigned)mask, (unsigned)pending_mask, (unsigned)best,
               (unsigned)STREAM_RING_SAMPLES, (unsigned)rs485_vq_sequence,
               (unsigned)StreamRing_LastUacSequence());
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (n < 0 || (size_t)n >= sizeof b)
    {
      RS485_Reply("err:buf\r\n");
      return;
    }
    n += snprintf(b + n, sizeof b - (size_t)n, " %u %u %u",
                  (unsigned)sessions[i], (unsigned)fills[i],
                  (unsigned)free_samples[i]);
  }
  if (n < 0 || (size_t)n >= sizeof b - 2u)
  {
    RS485_Reply("err:buf\r\n");
    return;
  }
  b[n++] = '\r';
  b[n++] = '\n';
  b[n] = '\0';
  if (console_via_usb)
  {
    RS485_Reply(b);
  }
  else
  {
    RS485_ReplyVq(mask, pending_mask, best, sessions, fills, free_samples);
  }
}

/** n off: release all voices. */
static void Console_CmdNoteAll(char *line)
{
  if (strcmp(line, "n off") != 0) { RS485_Reply("err:syntax\r\n"); return; }
  Console_CpuLoad_DisableVoices();RS485_Reply("ok\r\n");
}

/** n0..n7: note bank on CH1 (also answers unknown; slots 8..f err:range). */
static void Console_CmdNoteSlot(char *line)
{
  unsigned int key;
  unsigned int session;
  char extra;
  uint8_t note;
  int nscan;

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
  if (note >= NOTE_BANK_VOICES)
  {
    RS485_Reply("err:range\r\n");
    return;
  }

  /* Bare "n0" = session defaults only. Bare n1..n7 are not session cmds. */
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

  if (strcmp(line + 3, "off") == 0)
  {
    NoteBank_NoteOff(note);RS485_Reply("ok\r\n");return;
  }
  nscan = sscanf(line + 3, "on %u @%u %c", &key, &session, &extra);
  if (nscan == 2)
  {
    if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || session >= USB_STREAM_SESSION_MOD)
    { RS485_Reply("err:range\r\n"); return; }
  }
  else
  {
    nscan = sscanf(line + 3, "on %u %c", &key, &extra);
    if (nscan != 1) { RS485_Reply("err:syntax\r\n"); return; }
    if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT) { RS485_Reply("err:range\r\n"); return; }
    session = USB_STREAM_SESSION_MOD;
  }
  Console_NoteOn(note, (uint8_t)key, (uint16_t)session);
}

/** aw <voice> <id> — assign AXI head 0..255 to voice 0..7. */
static void Console_CmdAssignWave(char *line)
{
  unsigned int voice;
  unsigned int wid;

  if (sscanf(line, "aw %u %u", &voice, &wid) != 2)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  if (NoteBank_SetWaveId((uint8_t)voice, (uint16_t)wid) != 0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  {
    char b[48];
    snprintf(b, sizeof b, "ok: aw %u %u\r\n", voice, wid);
    RS485_Reply(b);
  }
}

/** a — loaded count + 256-bit hex mask (bit 0 = wave 0). */
static void Console_CmdAttackQuery(void)
{
  char b[96];
  uint8_t mask[32];
  int n;
  uint8_t i;

  AttackBank_LoadedMask(mask);
  n = snprintf(b, sizeof b, "ok: a %u ",
               (unsigned)AttackBank_LoadedCount());
  for (i = 0u; i < 32u && n > 0 && (size_t)n < sizeof b; i++)
  {
    n += snprintf(b + n, sizeof b - (size_t)n, "%02x", (unsigned)mask[i]);
  }
  if (n > 0 && (size_t)n < sizeof b)
  {
    snprintf(b + n, sizeof b - (size_t)n, "\r\n");
  }
  RS485_Reply(b);
}

/** al <wave_id> <nbytes> — CDC binary load (2..ATTACK_BANK_BYTES). */
static void Console_CmdAttackLoad(char *line)
{
  unsigned int wid;
  unsigned long nbytes;

  if (!console_via_usb)
  {
    RS485_Reply("err:usb\r\n");
    return;
  }
  if (sscanf(line, "al %u %lu", &wid, &nbytes) != 2)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  if (AttackUpload_Begin((uint16_t)wid, (uint32_t)nbytes) != 0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  RS485_Reply("ok:ready\r\n");
}

/** vmload <voice> <nbytes> — receive one Channel ABI6 FWSC container over CDC. */
static void Console_CmdVmLoad(char *line)
{
  unsigned int voice;
  unsigned long nbytes;
  if (!console_via_usb)
  {
    RS485_Reply("err:usb\r\n");
    return;
  }
  if (sscanf(line, "vmload %u %lu", &voice, &nbytes) != 2 ||
      voice >= FW_VM_CHANNEL_VOICE_COUNT)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  if (NoteBank_AnyActive() != 0u)
  {
    RS485_Reply("err:vm-busy\r\n");
    return;
  }
  if (AttackUpload_IsActive() != 0u || VmUpload_Begin((uint8_t)voice, (uint32_t)nbytes) != 0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  RS485_Reply("ok:ready\r\n");
}

static void Console_CmdVmStatus(char *line)
{
  char reply[192];
  unsigned int voice;
  if (strcmp(line, "vm mem") == 0)
  {
    const FwVmMemoryMetrics *m = NoteBank_VmMemoryMetrics();
    (void)snprintf(reply, sizeof(reply),
      "ok:vm mem %lu %lu %lu %lu %lu %lu %lu %lu %u\r\n",
      (unsigned long)m->arena_size, (unsigned long)m->arena_current,
      (unsigned long)m->arena_peak, (unsigned long)m->arena_largest_free,
      (unsigned long)m->load_allocations, (unsigned long)m->handler_allocations,
      (unsigned long)m->load_gc, (unsigned long)m->handler_gc,
      (unsigned)m->shared_vm_valid);
    RS485_Reply(reply);
    for (voice = 0u; voice < FW_VM_CHANNEL_VOICE_COUNT; ++voice)
    {
      (void)snprintf(reply, sizeof(reply), "ok:vm memv %u %u %lu %lu\r\n",
        voice, (unsigned)NoteBank_VmFault((uint8_t)voice),
        (unsigned long)NoteBank_VmMaxCycles((uint8_t)voice),
        (unsigned long)NoteBank_VmFaultCount((uint8_t)voice));
      RS485_Reply(reply);
    }
    return;
  }
  else if (strcmp(line, "vm") == 0)
  {
    (void)snprintf(reply, sizeof(reply), "ok:vm mask %02x\r\n",
                   (unsigned)NoteBank_VmActiveMask());
  }
  else if (sscanf(line, "vm %u", &voice) != 1 ||
           voice >= FW_VM_CHANNEL_VOICE_COUNT)
  {
    (void)snprintf(reply, sizeof(reply), "err:syntax\r\n");
  }
  else if (NoteBank_VmIsActive((uint8_t)voice) != 0u)
  {
    (void)snprintf(reply, sizeof(reply), "ok:vm %u active %lu %u %u\r\n",
                   voice, (unsigned long)FW_VM_TARGET_CHANNEL,
                   (unsigned)FW_VM_TARGET_CHANNEL_VERSION,
                   (unsigned)NoteBank_VmFault((uint8_t)voice));
  }
  else
  {
    (void)snprintf(reply, sizeof(reply), "ok:vm %u inactive %u\r\n",
                   voice, (unsigned)NoteBank_VmFault((uint8_t)voice));
  }
  RS485_Reply(reply);
}

/** bl is retired — body is the USB BODY stream, not on-card RAM. */
static void Console_CmdBodyLoad(char *line)
{
  (void)line;
  RS485_Reply("err:unsupported\r\n");
}

/** ar <wave_id> <root_hz> — attack-head native pitch for on-card rate-scale. */
static void Console_CmdRoot(char *line)
{
  unsigned int wid;
  float hz;

  if (sscanf(line, "ar %u %f", &wid, &hz) != 2)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  if (wid >= ATTACK_BANK_COUNT || !(hz > 0.0f))
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  AttackBank_SetRootHz((uint16_t)wid, hz);
  {
    char b[48];
    snprintf(b, sizeof b, "ok: ar %u %.6g\r\n", wid, (double)hz);
    RS485_Reply(b);
  }
}

static void Console_Exec(char *line)
{
  char b[160];
  unsigned int ch, val;

  if (line[0] == '\0')
  {
    return;
  }

  /* ---- h / help / ? ---- */
  if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 ||
      strcmp(line, "?") == 0)
  {
    Console_Help();
    return;
  }

  if (strncmp(line, "vmload ", 7) == 0)
  {
    Console_CmdVmLoad(line);
    return;
  }

  if (strcmp(line, "vm") == 0 || strncmp(line, "vm ", 3) == 0)
  {
    Console_CmdVmStatus(line);
    return;
  }

  /* ---- al <id> nbytes: CDC attack head upload ---- */
  if (strncmp(line, "al ", 3) == 0)
  {
    Console_CmdAttackLoad(line);
    return;
  }

  /* ---- bl: retired (body is USB-streamed) ---- */
  if (strncmp(line, "bl ", 3) == 0)
  {
    Console_CmdBodyLoad(line);
    return;
  }

  /* ---- ar <id> <hz>: sample root pitch ---- */
  if (strncmp(line, "ar ", 3) == 0)
  {
    Console_CmdRoot(line);
    return;
  }

  /* ---- aw <voice> <wave_id> ---- */
  if (strncmp(line, "aw ", 3) == 0)
  {
    Console_CmdAssignWave(line);
    return;
  }

  /* ---- a: loaded heads (slot = voice) ---- */
  if (strcmp(line, "a") == 0)
  {
    Console_CmdAttackQuery();
    return;
  }

  /* ---- vq: active-voice mask + exact per-voice free samples ---- */
  if (strcmp(line, "vq") == 0)
  {
    Console_CmdVoiceQuery();
    return;
  }

  /* ---- usb [0]: BODY counters (FIFO drop, hold, fill) ---- */
  if (strncmp(line, "usb", 3) == 0 && (line[3] == '\0' || line[3] == ' '))
  {
    if (line[3] == ' ')
    {
      unsigned z = 1u;
      if (sscanf(line + 4, "%u", &z) != 1 || z != 0u)
      {
        RS485_Reply("err:syntax\r\n");
        return;
      }
      Audio_Bridge_UsbDropCountClear();
      USB_App_StatsClear();
      rs485_vq_count = 0u;
    }
    {
      char min_s[12];
      uint32_t minf = StreamRing_MinFill();
      char usb_b[300];

      if (minf == 0xFFFFFFFFu)
      {
        min_s[0] = '-';
        min_s[1] = '\0';
      }
      else
      {
        snprintf(min_s, sizeof min_s, "%lu", (unsigned long)minf);
      }
      snprintf(usb_b, sizeof usb_b,
               "ok: usb drop %lu hold %lu min %s fill %lu "
               "z %lu sof %lu vq %lu win %lu rx %lu bytes %lu "
               "bad %lu h%lu s%lu f%lu c%lu u%lu late %lu "
               "stale %lu future %lu full %lu superseded %lu\r\n",
               (unsigned long)Audio_Bridge_UsbDropCount(),
               (unsigned long)NoteBank_HoldCount(), min_s,
               (unsigned long)Audio_Bridge_MaxFill(),
               (unsigned long)StreamRing_ZeroCount(),
               (unsigned long)StreamRing_SofCount(),
               (unsigned long)rs485_vq_count,
               (unsigned long)USB_App_UacWindowCount(),
               (unsigned long)USB_App_RxMsgCount(),
               (unsigned long)USB_App_RxByteCount(),
               (unsigned long)USB_App_BadCount(),
               (unsigned long)USB_App_BadReasonCount(0u),
               (unsigned long)USB_App_BadReasonCount(1u),
               (unsigned long)USB_App_BadReasonCount(2u),
               (unsigned long)USB_App_BadReasonCount(3u),
               (unsigned long)USB_App_BadReasonCount(4u),
               (unsigned long)Audio_Bridge_FillLate(),
               (unsigned long)StreamRing_StaleCount(),
               (unsigned long)StreamRing_FutureCount(),
               (unsigned long)StreamRing_FullCount(),
               (unsigned long)StreamRing_SupersededCount());
      RS485_Reply(usb_b);
    }
    return;
  }

  /* ---- fb: retired with UAC async feedback ---- */
  if (strncmp(line, "fb", 2) == 0 && (line[2] == '\0' || line[2] == ' '))
  {
    RS485_Reply("err:unsupported\r\n");
    return;
  }

  /* ---- s / p <0.1..0.9> / t <0.1..0.9> / saw: oscillator shape ---- */
  if (strcmp(line, "s") == 0)
  {
    (void)NoteBank_SetShape(NOTE_SHAPE_SINE, 0.0);
    Console_ShapeReply();
    return;
  }
  if (strcmp(line, "saw") == 0)
  {
    (void)NoteBank_SetShape(NOTE_SHAPE_SAW, 0.0);
    Console_ShapeReply();
    return;
  }
  if (line[0] == 'p' || line[0] == 't')
  {
    double param;
    NoteBank_Shape_t sh =
        (line[0] == 'p') ? NOTE_SHAPE_PULSE : NOTE_SHAPE_TRI;

    if (line[1] != ' ')
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    if (sscanf(line + 2, "%lf", &param) != 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    if (NoteBank_SetShape(sh, param) != 0)
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    Console_ShapeReply();
    return;
  }

  /* ---- g <ch> <dB>: CS4304 DAC atten ---- */
  if (sscanf(line, "g %u %u", &ch, &val) == 2)
  {
    (void)ch;
    (void)val;
    Console_CmdGain(line);
    return;
  }

  /* ---- fk / fk0..fk7: filter pitch-track (before bare f) ---- */
  if (line[0] == 'f' && line[1] == 'k')
  {
    Console_CmdFk(line, b, sizeof b);
    return;
  }

  /* ---- f0..f7 / f: LPF on first 8 voices; optional q = DF4 g ---- */
  if (line[0] == 'f')
  {
    Console_CmdFilter(line, b, sizeof b);
    return;
  }

  /* ---- cpu [0|N|q [N]]: LED_Y busy/idle probe ---- */
  if (strncmp(line, "cpu", 3) == 0 && (line[3] == '\0' || line[3] == ' '))
  {
    Console_CmdCpu(line, b, sizeof b);
    return;
  }

  /* ---- n off: silence all voices. n0..n7 below. ---- */
  if (line[0] == 'n' && (line[1] == '\0' || line[1] == ' '))
  {
    Console_CmdNoteAll(line);
    return;
  }

  /* ---- n0..n7: 8-voice note bank on CH1 (slots 8..f reply err:range) ---- */
  Console_CmdNoteSlot(line);
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
  if (line == NULL)
  {
    return;
  }
  if (!RS485_IsForMe(line)) /* accept "c:", "*:" or bare commands */
    return;
  console_via_usb = 1;
  Console_Exec(line);
  console_via_usb = 0;
}

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * ASCII only (fractional nX Hz). Messages for other cards are dropped. */
static void Console_Poll(void)
{
  static uint8_t cmd[96];
  static uint8_t idx = 0;
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
    if (c == '\r' || c == '\n')
    {
#if RS485_ECHO
      RS485_Send("\r\n");
#endif
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

/** Non-blocking fixed red/yellow chaser (150 ms per step).
 * The RGB package is exclusively VM-controlled. */
static void LED_Task(void)
{
  static GPIO_TypeDef *const ports[2] = {LED_R_GPIO_Port, LED_Y_GPIO_Port};
  static const uint16_t pins[2] = {LED_R_Pin, LED_Y_Pin};
  static uint32_t t_next = 0;
  static uint8_t step = 0;

  ChannelLed_Task();
  if (Audio_CpuLoad_IsActive())
  {
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_RESET);
    return;
  }
  if (!led_show_on)
  {
    for (uint8_t i = 0; i < 2; i++)
    {
      HAL_GPIO_WritePin(ports[i], pins[i], GPIO_PIN_RESET);
    }
    return;
  }
  if (HAL_GetTick() >= t_next)
  {
    t_next = HAL_GetTick() + 150;
    HAL_GPIO_WritePin(ports[step], pins[step], GPIO_PIN_RESET);
    step = (step + 1) % 2;
    HAL_GPIO_WritePin(ports[step], pins[step], GPIO_PIN_SET);
  }
}

void ChannelConsole_Init(void)
{
  ChannelLed_Init();
  RS485_BusRelease();
  Uart5Rx_Init();

  for (uint8_t i = 0; i < NUM_SWITCHES; i++)
  {
    HAL_GPIO_WritePin(switches[i].port, switches[i].pin,
                      switches[i].active_low ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  Console_ApplySessionDefaults();

  NoteFilter_InitAll();
  NoteEnv_Init();
  AttackBank_Init();
  StreamRing_Init();
  NoteBank_Init();

  RS485_Reply("ok: ready — h for cmds\r\n");
}

void ChannelConsole_Poll(void)
{
  Console_Poll();
  LED_Task();
}
