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
#include "audio_bridge.h"
#include "note_bank.h"
#include "note_envelope.h"
#include "note_filter.h"
#include "uart5_rx.h"
#include "usb_app.h"
#include "attack_bank.h"
#include "attack_upload.h"
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

/* 921600 8N1 ≈ 12 µs/byte. HAL Timeout is a deadline, not the payload
 * size. The 12-byte vq frame is ~150 µs on the wire; 50 ms was a poll
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

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied.
 *
 * Frame must hold the longest reply (~110 chars) — a short buffer silently
 * swallowed those ACKs and looked like a dead bus. Truncate rather than
 * drop: a clipped line still terminates the host's exchange. */
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
 * Fixed vq frame: sync[2], card, type, active mask, best voice,
 * four packed free-slot bytes, CRC-8, terminator. Keeping the status
 * exchange below one USB frame reduces the time this blocking UART write
 * can delay TinyUSB service.
 */
#define VQ_FRAME_LEN 12u
#define VQ_SYNC_0 0xA5u
#define VQ_SYNC_1 0x5Au
#define VQ_CARD_CHANNEL 0x43u
#define VQ_TYPE_STATUS 0x01u
_Static_assert(NOTE_BANK_VOICES == 8u,
               "vq binary frame packs exactly eight voices");

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

static void RS485_ReplyVq(uint8_t mask, uint8_t best,
                          const uint8_t *slots)
{
  uint8_t frame[VQ_FRAME_LEN];
  uint8_t i;

  frame[0] = VQ_SYNC_0;
  frame[1] = VQ_SYNC_1;
  frame[2] = VQ_CARD_CHANNEL;
  frame[3] = VQ_TYPE_STATUS;
  frame[4] = mask;
  frame[5] = best;
  for (i = 0u; i < NOTE_BANK_VOICES; i += 2u)
  {
    frame[6u + (i / 2u)] =
        (uint8_t)((slots[i] & 0x0Fu) | ((slots[i + 1u] & 0x0Fu) << 4u));
  }
  frame[10] = RS485_Crc8(frame, 10u);
  frame[11] = '\n';

  RS485_BusAcquire();
  if (HAL_UART_Transmit(&huart5, frame, VQ_FRAME_LEN,
                        RS485_TxDeadlineMs(VQ_FRAME_LEN)) != HAL_OK)
  {
    rs485_tx_fail++;
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
 *   n0..n7 <Hz> [sc]  — note freq; optional scale 0.0..1.0 (default 0.125)
 *   n <Hz> [sc]       — all 8 voices (n 0 = silence)
 *   s / p <d> / t <a> — global note-bank shape (sine / pulse duty / tri asym)
 *   al <id> <len>     — CDC attack-head upload (2..ATTACK_BANK_BYTES)
 *   ar <id> <Hz>      — sample root pitch (id = wave 0..255); a — loaded mask
 *   a / vq            — loaded heads per voice / hungriest + free slots
 *   interp [0|1]      — 0 = nearest sample, 1 = 2-tap linear (default)
 *   usb               — BODY counters: drop/hold/min/fill/z/sof/rx/bytes/bad
 *   usb 0             — clear those counters, then same reply
 *   en0..enf / en     — envelope: end slope[±k] … release_slope[±k]
 *                       (en0 0 / en 0 = clear → unprogrammed bypass)
 *   ek0..ekf / ek     — env pitch-track k (−10..10, rate ∝ (f/C4)^k)
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
 * Frequency changes (`nX <Hz>`) do not touch gain or bypass. */
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
           "ok: SAMPLE n0..n7 Hz [sc] | s|p d|t a | aw v id | "
           "al id n | ar id Hz | a | vq | interp 0|1 | usb | "
           "en0..en7 end slope[±k] ... rel[±k]|0 | ek0..ek7 k | "
           "f0..f7 Hz [q] | fk0..fk7 k | g ch dB | "
           "cpu [0|N|q N]\r\n");
  RS485_Reply(b);
}

/** Hex slot char for replies (0..15 → '0'..'9','a'..'f'). */
static char Console_NoteSlotChar(uint8_t note)
{
  if (note < 10u)
  {
    return (char)('0' + note);
  }
  return (char)('a' + (note - 10u));
}

/** Append " %.2f" or " %.2f%+.2f" when k != 0. */
static int Console_AppendSlopeK(char *b, size_t bsz, int n, float slope, float k)
{
  if (n < 0 || (size_t)n >= bsz)
  {
    return n;
  }
  if (k != 0.0f)
  {
    return n + snprintf(b + n, bsz - (size_t)n, " %.2f%+.2f", (double)slope,
                        (double)k);
  }
  return n + snprintf(b + n, bsz - (size_t)n, " %.2f", (double)slope);
}

static void Console_EnvReply(uint8_t voice)
{
  char b[280];
  int n;
  uint8_t nseg;
  uint8_t i;
  uint8_t release_idx;

  nseg = NoteEnv_GetSegmentCount(voice);
  if (nseg < NOTE_ENV_SEGMENTS_MIN)
  {
    snprintf(b, sizeof b, "ok: en%c (none)\r\n", Console_NoteSlotChar(voice));
    RS485_Reply(b);
    return;
  }

  /* Echo console form: end slope[±k] … release_slope[±k]. */
  n = snprintf(b, sizeof b, "ok: en%c", Console_NoteSlotChar(voice));
  release_idx = (uint8_t)(nseg - 1u);
  for (i = 0; i < release_idx && n > 0 && (size_t)n < sizeof b; i++)
  {
    NoteEnv_Segment_t seg;
    if (NoteEnv_GetSegment(voice, i, &seg) != 0)
    {
      break;
    }
    n += snprintf(b + n, sizeof b - (size_t)n, " %.2f", (double)seg.end_amp);
    n = Console_AppendSlopeK(b, sizeof b, n, seg.slope, seg.k);
  }
  if (n > 0 && (size_t)n < sizeof b)
  {
    NoteEnv_Segment_t rel;
    if (NoteEnv_GetSegment(voice, release_idx, &rel) == 0)
    {
      n = Console_AppendSlopeK(b, sizeof b, n, rel.slope, rel.k);
    }
  }
  if (n > 0 && (size_t)n < sizeof b)
  {
    snprintf(b + n, sizeof b - (size_t)n, "\r\n");
  }
  RS485_Reply(b);
}

static void Console_EkReply(uint8_t voice)
{
  char b[120];
  uint8_t nseg;
  uint8_t i;
  int n;
  float k0;
  uint8_t same;

  nseg = NoteEnv_GetSegmentCount(voice);
  if (nseg < NOTE_ENV_SEGMENTS_MIN)
  {
    snprintf(b, sizeof b, "ok: ek%c %.1f\r\n", Console_NoteSlotChar(voice),
             (double)NoteEnv_GetPitchK(voice));
    RS485_Reply(b);
    return;
  }

  same = 1u;
  {
    NoteEnv_Segment_t seg0;
    if (NoteEnv_GetSegment(voice, 0u, &seg0) != 0)
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    k0 = seg0.k;
    for (i = 1u; i < nseg; i++)
    {
      NoteEnv_Segment_t seg;
      if (NoteEnv_GetSegment(voice, i, &seg) != 0 || seg.k != k0)
      {
        same = 0u;
        break;
      }
    }
  }
  if (same != 0u)
  {
    snprintf(b, sizeof b, "ok: ek%c %.1f\r\n", Console_NoteSlotChar(voice),
             (double)k0);
    RS485_Reply(b);
    return;
  }

  n = snprintf(b, sizeof b, "ok: ek%c", Console_NoteSlotChar(voice));
  for (i = 0u; i < nseg && n > 0 && (size_t)n < sizeof b; i++)
  {
    NoteEnv_Segment_t seg;
    if (NoteEnv_GetSegment(voice, i, &seg) != 0)
    {
      break;
    }
    n += snprintf(b + n, sizeof b - (size_t)n, " %.1f", (double)seg.k);
  }
  if (n > 0 && (size_t)n < sizeof b)
  {
    snprintf(b + n, sizeof b - (size_t)n, "\r\n");
  }
  RS485_Reply(b);
}

/**
 * Parse one whitespace-delimited token as a plain float (no trailing junk).
 * Returns 0 ok, -1 syntax.
 */
static int Console_ParsePlainFloatToken(const char *tok, float *out)
{
  char *end = NULL;
  double v;

  if (tok == NULL || *tok == '\0' || out == NULL)
  {
    return -1;
  }
  v = strtod(tok, &end);
  if (end == tok || *end != '\0')
  {
    return -1;
  }
  *out = (float)v;
  return 0;
}

/**
 * Parse slope[±k]: "10", "10+2", "2.0-1.5". No spaces inside the token.
 * Returns 0 ok, -1 syntax.
 */
static int Console_ParseSlopeKToken(const char *tok, float *slope_out,
                                    float *k_out)
{
  char *end = NULL;
  double slope;
  float k = 0.0f;

  if (tok == NULL || *tok == '\0' || slope_out == NULL || k_out == NULL)
  {
    return -1;
  }
  slope = strtod(tok, &end);
  if (end == tok)
  {
    return -1;
  }
  if (*end == '+' || *end == '-')
  {
    char *kend = NULL;
    double kd = strtod(end, &kend);
    if (kend == end || *kend != '\0')
    {
      return -1;
    }
    k = (float)kd;
  }
  else if (*end != '\0')
  {
    return -1;
  }
  *slope_out = (float)slope;
  *k_out = k;
  return 0;
}

/**
 * Copy next whitespace-delimited token into tok. Advances *pp past it.
 * Returns 0 ok, -1 if no token or token too long.
 */
static int Console_NextToken(const char **pp, char *tok, size_t tok_sz)
{
  const char *p;
  size_t len = 0u;

  if (pp == NULL || *pp == NULL || tok == NULL || tok_sz < 2u)
  {
    return -1;
  }
  p = *pp;
  while (*p == ' ')
  {
    p++;
  }
  if (*p == '\0')
  {
    return -1;
  }
  while (p[len] != '\0' && p[len] != ' ')
  {
    len++;
  }
  if (len >= tok_sz)
  {
    return -1;
  }
  memcpy(tok, p, len);
  tok[len] = '\0';
  *pp = p + len;
  return 0;
}

/**
 * Parse en program: end slope[±k] [end slope[±k] ...] release_slope[±k].
 * Single token 0 → NoteEnv_Clear (unprogrammed bypass, *nseg_out = 0).
 * Odd token count 3..NOTE_ENV_CONSOLE_FLOATS_MAX.
 * Returns 0 ok, -1 syntax/count, -2 range (SetSegments).
 */
static int Console_EnvApplyProgram(uint8_t voice, const char *rest,
                                   uint8_t *nseg_out)
{
  NoteEnv_Segment_t segs[NOTE_ENV_SEGMENTS_MAX];
  char tok[32];
  const char *p = rest;
  const char *count_p = rest;
  uint8_t ntok = 0u;
  uint8_t n_pre;
  uint8_t nseg;
  uint8_t i;
  int rc;

  /* Count tokens first. */
  while (Console_NextToken(&count_p, tok, sizeof tok) == 0)
  {
    ntok++;
    if (ntok > NOTE_ENV_CONSOLE_FLOATS_MAX)
    {
      return -1;
    }
  }
  while (*count_p == ' ')
  {
    count_p++;
  }
  if (*count_p != '\0')
  {
    return -1; /* token too long or junk */
  }

  /* en 0 / en0 0 → clear to bypass. */
  if (ntok == 1u)
  {
    float clear_v;

    if (Console_NextToken(&p, tok, sizeof tok) != 0 ||
        Console_ParsePlainFloatToken(tok, &clear_v) != 0 || clear_v != 0.0f)
    {
      return -1;
    }
    while (*p == ' ')
    {
      p++;
    }
    if (*p != '\0')
    {
      return -1;
    }
    NoteEnv_Clear(voice);
    if (nseg_out != NULL)
    {
      *nseg_out = 0u;
    }
    return 0;
  }

  if (ntok < 3u || (ntok % 2u) == 0u)
  {
    return -1;
  }

  n_pre = (uint8_t)((ntok - 1u) / 2u);
  nseg = (uint8_t)(n_pre + 1u);
  if (nseg < NOTE_ENV_SEGMENTS_MIN || nseg > NOTE_ENV_SEGMENTS_MAX)
  {
    return -1;
  }

  for (i = 0u; i < n_pre; i++)
  {
    if (Console_NextToken(&p, tok, sizeof tok) != 0 ||
        Console_ParsePlainFloatToken(tok, &segs[i].end_amp) != 0)
    {
      return -1;
    }
    if (Console_NextToken(&p, tok, sizeof tok) != 0 ||
        Console_ParseSlopeKToken(tok, &segs[i].slope, &segs[i].k) != 0)
    {
      return -1;
    }
  }
  if (Console_NextToken(&p, tok, sizeof tok) != 0)
  {
    return -1;
  }
  segs[n_pre].end_amp = 0.0f;
  if (Console_ParseSlopeKToken(tok, &segs[n_pre].slope, &segs[n_pre].k) != 0)
  {
    return -1;
  }
  while (*p == ' ')
  {
    p++;
  }
  if (*p != '\0')
  {
    return -1;
  }

  rc = NoteEnv_SetSegments(voice, segs, nseg);
  if (rc == 0 && nseg_out != NULL)
  {
    *nseg_out = nseg;
  }
  return rc;
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

/** en / en0..enf: multi-segment amplitude envelope. */
static void Console_CmdEnv(char *line, char *b, size_t bsz)
{
  const char *rest;
  int rc;
  uint8_t note;
  uint8_t nseg = 0u;

  if (line[2] == '\0' || line[2] == ' ')
  {
    rest = line + 2;
    while (*rest == ' ')
    {
      rest++;
    }
    if (*rest == '\0')
    {
      /* Compact dump of all programmed voices. */
      int n = snprintf(b, bsz, "ok:");
      for (uint8_t i = 0; i < NOTE_BANK_VOICES && n > 0 && (size_t)n < bsz;
           i++)
      {
        if (NoteEnv_IsProgrammed(i) != 0u)
        {
          n += snprintf(b + n, bsz - (size_t)n, " en%c",
                        Console_NoteSlotChar(i));
        }
      }
      if (n == (int)strlen("ok:"))
      {
        snprintf(b, bsz, "ok: en (none)\r\n");
      }
      else if ((size_t)n < bsz - 2u)
      {
        snprintf(b + n, bsz - (size_t)n, "\r\n");
      }
      RS485_Reply(b);
      return;
    }
    for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
    {
      rc = Console_EnvApplyProgram(i, rest, &nseg);
      if (rc == -1)
      {
        RS485_Reply("err:syntax\r\n");
        return;
      }
      if (rc != 0)
      {
        RS485_Reply("err:range\r\n");
        return;
      }
    }
    if (nseg == 0u)
    {
      snprintf(b, bsz, "ok: en (none)\r\n");
    }
    else
    {
      snprintf(b, bsz, "ok: en (%u seg)\r\n", (unsigned)nseg);
    }
    RS485_Reply(b);
    return;
  }

  note = Console_ParseNoteSlot(line[2]);
  if (note == 0xFFu || (line[3] != '\0' && line[3] != ' '))
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  rest = line + 3;
  while (*rest == ' ')
  {
    rest++;
  }
  if (*rest == '\0')
  {
    Console_EnvReply(note);
    return;
  }
  rc = Console_EnvApplyProgram(note, rest, &nseg);
  if (rc == -1)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  if (rc != 0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  Console_EnvReply(note);
}

/** ek / ek0..ekf: pitch-track constant. */
static void Console_CmdEk(char *line, char *b, size_t bsz)
{
  const char *rest;
  double kd;
  float k;
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
      for (uint8_t i = 0; i < NOTE_BANK_VOICES && n > 0 && (size_t)n < bsz;
           i++)
      {
        n += snprintf(b + n, bsz - (size_t)n, " ek%c=%.1f",
                      Console_NoteSlotChar(i), (double)NoteEnv_GetPitchK(i));
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
    k = (float)kd;
    for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
    {
      rc = NoteEnv_SetPitchK(i, k);
      if (rc != 0)
      {
        RS485_Reply("err:range\r\n");
        return;
      }
    }
    snprintf(b, bsz, "ok: ek %.1f\r\n", (double)k);
    RS485_Reply(b);
    return;
  }

  note = Console_ParseNoteSlot(line[2]);
  if (note == 0xFFu || (line[3] != '\0' && line[3] != ' '))
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  rest = line + 3;
  while (*rest == ' ')
  {
    rest++;
  }
  if (*rest == '\0')
  {
    Console_EkReply(note);
    return;
  }
  if (sscanf(rest, "%lf", &kd) != 1)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }
  k = (float)kd;
  rc = NoteEnv_SetPitchK(note, k);
  if (rc != 0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  Console_EkReply(note);
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
 * vq — hungriest voice + free slot counts (bench / RS485).
 * Reply: ok:vq <mask_hex> <best> <s0>..<s7>
 *   mask bit i = NoteBank_IsActive; best = 0..7 or 255.
 *   si = 0..14 free 256-sample slots; 15 means the ring is empty.
 */
static void Console_CmdVoiceQuery(void)
{
  char b[96];
  int n;
  uint8_t mask = 0u;
  uint8_t best = 0xFFu;
  uint8_t slots[NOTE_BANK_VOICES];
  uint8_t i;

  NoteBank_VoiceQuery(&mask, &best, slots);

  n = snprintf(b, sizeof b, "ok:vq %02x %u", (unsigned)mask, (unsigned)best);
  for (i = 0u; i < NOTE_BANK_VOICES; i++)
  {
    if (n < 0 || (size_t)n >= sizeof b)
    {
      RS485_Reply("err:buf\r\n");
      return;
    }
    n += snprintf(b + n, sizeof b - (size_t)n, " %u", (unsigned)slots[i]);
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
    RS485_ReplyVq(mask, best, slots);
  }
}

/** n <Hz> [scale]: all voices (n 0 = silence). */
static void Console_CmdNoteAll(char *line)
{
  const char *rest = line + 1;
  double hz;
  double scale;
  int nscan;

  while (*rest == ' ')
  {
    rest++;
  }
  if (*rest == '\0')
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }

  nscan = sscanf(rest, "%lf %lf", &hz, &scale);
  if (nscan == 1)
  {
    scale = NOTE_DEFAULT_SCALE;
  }
  else if (nscan != 2)
  {
    RS485_Reply("err:syntax\r\n");
    return;
  }

  if (hz <= 0.0)
  {
    Console_CpuLoad_DisableVoices();
    RS485_Reply("ok\r\n");
    return;
  }
  if (hz < 20.0 || hz >= 20000.0 || scale < 0.0 || scale > 1.0)
  {
    RS485_Reply("err:range\r\n");
    return;
  }
  for (uint8_t i = 0; i < NOTE_BANK_VOICES; i++)
  {
    NoteBank_SetFreq(i, hz, scale);
  }
  RS485_Reply("ok\r\n");
}

/** n0..n7: note bank on CH1 (also answers unknown; slots 8..f err:range). */
static void Console_CmdNoteSlot(char *line)
{
  double hz;
  double scale;
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

  /* ---- vq: active-voice mask + per-voice ring fill quarters ---- */
  if (strcmp(line, "vq") == 0)
  {
    Console_CmdVoiceQuery();
    return;
  }

  /* ---- interp [0|1]: nearest-neighbour vs 2-tap linear (scope A/B) ---- */
  if (strncmp(line, "interp", 6) == 0 && (line[6] == '\0' || line[6] == ' '))
  {
    unsigned v = 0u;

    if (line[6] == '\0')
    {
      snprintf(b, sizeof b, "ok: interp %u\r\n",
               (unsigned)NoteBank_GetInterp());
      RS485_Reply(b);
      return;
    }
    if (sscanf(line + 7, "%u", &v) != 1)
    {
      RS485_Reply("err:syntax\r\n");
      return;
    }
    if (v > 1u || NoteBank_SetInterp((uint8_t)v) != 0)
    {
      RS485_Reply("err:range\r\n");
      return;
    }
    snprintf(b, sizeof b, "ok: interp %u\r\n", v);
    RS485_Reply(b);
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
    }
    {
      char min_s[12];
      uint32_t minf = StreamRing_MinFill();
      char usb_b[192];

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
               "z %lu sof %lu vq %lu rx %lu bytes %lu bad %lu late %lu\r\n",
               (unsigned long)Audio_Bridge_UsbDropCount(),
               (unsigned long)NoteBank_HoldCount(), min_s,
               (unsigned long)Audio_Bridge_MaxFill(),
               (unsigned long)StreamRing_ZeroCount(),
               (unsigned long)StreamRing_SofCount(),
               (unsigned long)USB_App_StatusCount(),
               (unsigned long)USB_App_RxMsgCount(),
               (unsigned long)USB_App_RxByteCount(),
               (unsigned long)USB_App_BadCount(),
               (unsigned long)Audio_Bridge_FillLate());
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

  /* ---- s / p <0.1..0.9> / t <0.1..0.9>: global note-bank shape ---- */
  if (strcmp(line, "s") == 0)
  {
    (void)NoteBank_SetShape(NOTE_SHAPE_SINE, 0.0);
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

  /* ---- en / en0..enf: multi-segment amplitude envelope ---- */
  if (line[0] == 'e' && line[1] == 'n')
  {
    Console_CmdEnv(line, b, sizeof b);
    return;
  }

  /* ---- ek / ek0..ekf: envelope pitch-track constant ---- */
  if (line[0] == 'e' && line[1] == 'k')
  {
    Console_CmdEk(line, b, sizeof b);
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

  /* ---- n <Hz> [scale]: all voices (n 0 = silence). n0..n7 below. ---- */
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
