#include "channel.h"
#include "audio.h"
#include "board.h"
#include "voice.h"
#include "filter.h"
#include "samples.h"
#include "stream.h"
#include "usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- drive the status leds ---------------------------------------------- */

/* The RGB pins are plain GPIOs, so brightness uses 4-bit, 1 kHz software PWM.
 * That gives a 62.5 Hz frame while keeping ChannelConsole_Poll non-blocking. */

static volatile uint8_t s_level[3];
static uint8_t s_phase;
static volatile uint8_t s_controlled;
static uint32_t s_last_tick;

/* ---- initialize the rgb led --------------------------------------------- */

void ChannelLed_Init(void)
{
    s_level[0] = s_level[1] = s_level[2] = 0u;
    s_phase = 0u;
    s_controlled = 0u;
    s_last_tick = Board_Millis();
}

/* ---- convert normalized led levels -------------------------------------- */

static uint8_t led_level(float color, float brightness)
{
    float scaled = color * brightness * 16.0f;
    return (uint8_t)(scaled + 0.5f);
}

/* ---- set the rgb led ---------------------------------------------------- */

int ChannelLed_Set(float red, float green, float blue, float brightness)
{
    s_level[0] = led_level(red, brightness);
    s_level[1] = led_level(green, brightness);
    s_level[2] = led_level(blue, brightness);
    s_controlled = 1u;
    return 0;
}

/* ---- apply pending led changes ------------------------------------------ */

void ChannelLed_Task(void)
{
    uint32_t now;
    uint8_t i;

    if (!s_controlled) return;
    now = Board_Millis();
    if (now == s_last_tick) return;
    s_last_tick = now;
    s_phase = (uint8_t)((s_phase + 1u) & 0x0fu);
    for (i = 0u; i < 3u; ++i)
        Board_LedWrite((uint8_t)(BOARD_LED_RGB_R + i), s_phase < s_level[i] ? 1u : 0u);
}

/* ---- process console commands ------------------------------------------- */

/* Channel Card RS485 console and status LEDs.
 * main.c owns bring-up and the DAC handle; this file owns the interactive
 * console surface. */

/* ---- multi-drop rs485 card addressing ----------------------------------- */

#define RS485_CARD_ID 'c'
#define RS485_BROADCAST_ID '*'
#define RS485_BUS_TIMEOUT_MS 250
#define RS485_ECHO 0

/* Keep blocking TX close to its wire time so USB audio can be serviced. */
static uint32_t RS485_TxDeadlineMs(uint32_t nbytes)
{
    return fw_rs485_tx_deadline_ms(nbytes, Board_Rs485Baud());
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

/** Wait up to timeout_ms for the bus to become free.  Returns 1 if free. */
static uint8_t RS485_WaitBusFree(uint32_t timeout_ms)
{
    uint32_t t0 = Board_Millis();
    while (!Board_Rs485BusFree()) {
        if ((Board_Millis() - t0) >= timeout_ms) return 0; /* timed out — bus still busy */
    }
    return 1;
}

/** Transmit a string on the RS485 bus with collision avoidance.
 * Waits for the bus to be free, asserts DE, sends data, releases DE.
 * Returns 0 on success, -1 if the bus was busy (timeout). */
static int __attribute__((unused)) RS485_Send(const char *s)
{
    if (!RS485_WaitBusFree(RS485_BUS_TIMEOUT_MS)) return -1; /* bus occupied — drop this message */

    Board_Rs485Acquire();

    {
        uint32_t n = (uint32_t)strlen(s);
        Uart5Rx_Transmit((const uint8_t *)s, (uint16_t)n, RS485_TxDeadlineMs(n));
    }

    Board_Rs485Release();
    return 0;
}

/* When a command arrives over the USB CDC console, replies must go back
 * over USB instead of the RS485 bus.  Console_ExecFromUSB() sets this
 * for the duration of the command. */
/* Main-loop parser state; reset is also available from the USB console. */
static uint8_t rs485_line[96];
static uint8_t rs485_line_len;

static uint8_t console_via_usb = 0;
static uint8_t console_note_event;

/* RS485 path counters (internal; no console cmd). */
static uint32_t rs485_cmd_count; /**< commands executed from the bus */
static uint32_t rs485_tx_fail;   /**< HAL_UART_Transmit did not return OK */
static uint32_t rs485_tx_trunc;  /**< reply longer than the frame buffer */
static uint32_t rs485_vq_count;  /**< exact refill-status frames transmitted */

/* ---- send tagged console replies ---------------------------------------- */

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied.
 *
 * Frame must hold the longest reply (~110 chars) — a short buffer silently
 * swallowed those ACKs and looked like a dead bus. Truncate rather than
 * drop: a clipped line still terminates the host's exchange. */
static void RS485_Reply(const char *s)
{
    if (console_note_event) return;
    char frame[224];
    size_t tag_len;
    size_t body_len;
    size_t max_body;

    if (console_via_usb) {
        USB_CDC_WriteStr(s);
        return;
    }

    tag_len = strlen(RS485_TAG);
    body_len = strlen(s);
    max_body = sizeof(frame) - tag_len - 1u;
    if (body_len > max_body) {
        body_len = max_body;
        rs485_tx_trunc++;
    }
    memcpy(frame, RS485_TAG, tag_len);
    memcpy(frame + tag_len, s, body_len);

    Board_Rs485Acquire();
    if (Uart5Rx_Transmit((const uint8_t *)frame, (uint16_t)(tag_len + body_len),
                         RS485_TxDeadlineMs((uint32_t)(tag_len + body_len))) != 0) {
        rs485_tx_fail++;
    }
    Board_Rs485Release();
}

/*
 * Sequenced vq frame: masks/capacity/status and last processed BODY sequence,
 * plus eight session/free-space/refill-budget/duration records and a terminator; no CRC.
 */
#define VQ_FRAME_LEN 61u
static_assert(STREAM_RING_SAMPLES <= 4095u, "vq format uses 12-bit physical free space");
#define VQ_SYNC_0 0xA5u
#define VQ_SYNC_1 0x5Au
#define VQ_CARD_CHANNEL 0x43u
#define VQ_TYPE_STATUS 0x0Eu
static_assert(NOTE_BANK_VOICES == 8u, "vq binary frame packs exactly eight voices");

static uint16_t rs485_vq_sequence;

/* ---- send packed voice status ------------------------------------------- */

static void RS485_ReplyVq(uint8_t active_mask, uint8_t pending_mask, const uint8_t *sessions,
                          const uint8_t *current_sessions, const uint32_t *remaining_us,
                          const uint16_t *free_samples, uint16_t body_sequence, uint8_t body_age_ms,
                          const uint16_t *refill_samples)
{
    uint8_t frame[VQ_FRAME_LEN] = {};
    frame[0] = VQ_SYNC_0;
    frame[1] = VQ_SYNC_1;
    frame[2] = VQ_CARD_CHANNEL;
    frame[3] = VQ_TYPE_STATUS;
    frame[4] = active_mask;
    frame[5] = pending_mask;
    frame[6] = (uint8_t)STREAM_RING_SAMPLES;
    frame[7] = (uint8_t)(STREAM_RING_SAMPLES >> 8u);
    ++rs485_vq_sequence;
    frame[8] = (uint8_t)rs485_vq_sequence;
    frame[9] = body_age_ms;
    frame[10] = (uint8_t)body_sequence;
    frame[11] = (uint8_t)(body_sequence >> 8u);
    for (uint8_t i = 0u; i < NOTE_BANK_VOICES; ++i) {
        uint8_t *record = frame + 12u + 6u * i;
        uint32_t duration = remaining_us[i] / 100u;
        if (duration > 2047u) duration = 2047u;
        record[0] = sessions[i];
        record[1] = current_sessions[i];
        record[2] = (uint8_t)free_samples[i];
        record[3] = (uint8_t)((free_samples[i] >> 8u) | ((refill_samples[i] & 15u) << 4u));
        record[4] = (uint8_t)((refill_samples[i] >> 4u) | ((duration & 7u) << 5u));
        record[5] = (uint8_t)(duration >> 3u);
    }
    frame[VQ_FRAME_LEN - 1u] = '\n';

    Board_Rs485Acquire();
    if (Uart5Rx_Transmit(frame, VQ_FRAME_LEN, RS485_TxDeadlineMs(VQ_FRAME_LEN)) != 0) {
        rs485_tx_fail++;
    } else {
        rs485_vq_count++;
    }
    Board_Rs485Release();
}

/* ---- channel card control console (rs485) ------------------------------- */

static uint8_t led_show_on = 1;

/* Analog-switch GPIO table. Driven once at init (all OFF, then the
 * session default turns bypass ON); there is no runtime switch command. */
typedef struct {
    const char *name;
    uint8_t id;
    uint8_t active_low; /* 1 = LOW enables the switch (ON) */
} SwitchDef_t;

static const SwitchDef_t switches[] = {
    {"hp", BOARD_SWITCH_HP, 1},
    {"bp", BOARD_SWITCH_BP, 1},
    {"lp", BOARD_SWITCH_LP, 1},
    {"vca", BOARD_SWITCH_VCA, 1},
    {"bypass", BOARD_SWITCH_BYPASS, 1},
    {"vcf", BOARD_SWITCH_VCF, 1},
    {"scf", BOARD_SWITCH_SCF, 1},
    {"hp_ctl", BOARD_SWITCH_HP_CTL, 0},
};
#define NUM_SWITCHES (sizeof(switches) / sizeof(switches[0]))

/* RS485 commands are parsed below. Uploads use the binary USB protocol. */
#define BOOT_DEFAULT_ATTEN_DB 0u
/** Console-addressable filter slots. */
#define NOTE_FILTER_CONSOLE_VOICES 8u

/* ---- apply analog switch defaults --------------------------------------- */

/** Bypass is active-low (LOW = ON). */
static void Console_SetBypassOn(void)
{
    Board_SwitchWrite(BOARD_SWITCH_BYPASS, 0u);
}

/** Boot defaults: dry path + CH1 DAC trim 0 dB (`g 1 0`).
 * Runtime note commands do not touch gain or bypass. */
static void Console_ApplyBootDefaults(void)
{
    Console_SetBypassOn();
    if (Board_DacReady() != 0u) {
        Board_DacSetTrim('1', (uint8_t)(BOOT_DEFAULT_ATTEN_DB * 2u));
    }
}

/* ---- parse voice selection ---------------------------------------------- */

/** Map hex digit '0'..'9','a'..'f' → 0..15; else 0xFF. */
static uint8_t Console_ParseNoteSlot(char hex_digit)
{
    if (hex_digit >= '0' && hex_digit <= '9') {
        return (uint8_t)(hex_digit - '0');
    }
    if (hex_digit >= 'a' && hex_digit <= 'f') {
        return (uint8_t)(10u + (uint8_t)(hex_digit - 'a'));
    }
    return 0xFFu;
}

/** Apply nX on <key> <velocity> [@session]. */
static void Console_NoteOn(uint8_t note, uint8_t key, uint8_t velocity, uint16_t session,
                           uint16_t sample)
{
    if (NoteBank_VmUploadIsBusy() != 0u) {
        RS485_Reply("err:vm-busy\r\n");
        return;
    }
    if (NoteBank_VmIsActive(note) == 0u) {
        RS485_Reply("err:no-program\r\n");
        return;
    }

    int result;
    if (sample != UINT16_MAX)
        result = NoteBank_NoteOnSampleSession(note, sample, key, velocity, (uint8_t)session);
    else if (session < USB_STREAM_SESSION_MOD)
        result = NoteBank_NoteOnSession(note, key, velocity, (uint8_t)session);
    else
        result = NoteBank_NoteOn(note, key, velocity);
    RS485_Reply(result == 0    ? "ok\r\n"
                : result == -3 ? "err:busy\r\n"
                : result == -2 ? "err:no-program\r\n"
                               : "err:range\r\n");
}

/** Hard-stop every voice and reset Berry state without unloading programs. */
static void Console_AllNotesOff(void)
{
    NoteBank_AllNotesOff();
}

/* ---- describe console commands ------------------------------------------ */

static void Console_Help(void)
{
    char b[320];
    /* One tagged line — leading \\r\\n would make the host see bare "[C]". */
    snprintf(b, sizeof b,
             "ok: reset (RS485 RX) | SAMPLE n0..n7 on sample key velocity @session | on key "
             "[velocity] [@session] | off | "
             "clear | n off | al id n | wl wave n | vmload v n | vm [v] | ar id Hz | a | vq | "
             "usb | cpuload [0|1] | "
             "f0..f7 Hz [q] | fk0..fk7 k | g ch dB\r\n");
    RS485_Reply(b);
}

/* ---- report filter settings --------------------------------------------- */

static void Console_FilterReply(uint8_t voice)
{
    char b[128];
    double base = NoteFilter_GetBaseCutoff(voice);
    double fc = NoteFilter_GetCutoff(voice);
    double k = NoteFilter_GetPitchK(voice);

    if (fc >= NOTE_FILTER_CUTOFF_MAX_HZ) {
        snprintf(b, sizeof b, "ok: f%u %.1f Hz (bypass) k %.2f q %.2f\r\n", (unsigned)voice, base,
                 k, NoteFilter_GetQ(voice));
    } else if (k > 0.0) {
        snprintf(b, sizeof b, "ok: f%u base %.1f → %.1f Hz k %.2f q %.2f\r\n", (unsigned)voice,
                 base, fc, k, NoteFilter_GetQ(voice));
    } else {
        snprintf(b, sizeof b, "ok: f%u %.1f Hz k %.2f q %.2f\r\n", (unsigned)voice, fc, k,
                 NoteFilter_GetQ(voice));
    }
    RS485_Reply(b);
}

static void Console_FkReply(uint8_t voice)
{
    char b[48];
    snprintf(b, sizeof b, "ok: fk%u %.2f\r\n", (unsigned)voice, NoteFilter_GetPitchK(voice));
    RS485_Reply(b);
}

/* ---- handle dac gain commands ------------------------------------------- */

/** g <ch> <dB>: CS4304 DAC atten. Caller already matched sscanf. */
static void Console_CmdGain(char *line)
{
    unsigned int ch, val;

    (void)sscanf(line, "g %u %u", &ch, &val);
    if (ch >= 1 && ch <= 4 && val <= 127 && Board_DacReady() != 0u) {
        Board_DacSetTrim((char)('0' + ch), (uint8_t)(val * 2u));
        RS485_Reply("ok\r\n");
    } else {
        RS485_Reply("err:range\r\n");
    }
}

/* ---- handle filter commands --------------------------------------------- */

/** f0..f7 / f: LPF on first 8 voices; optional q = DF4 g. */
static void Console_CmdFilter(char *line, char *b, size_t bsz)
{
    const char *rest;
    double fc;
    double q;
    int rc;
    int nscan;
    uint8_t note;

    if (line[1] >= '0' && line[1] <= '7' && (line[2] == '\0' || line[2] == ' ')) {
        note = (uint8_t)(line[1] - '0');
        rest = line + 2;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            Console_FilterReply(note);
            return;
        }
        nscan = sscanf(rest, "%lf %lf", &fc, &q);
        if (nscan < 1) {
            RS485_Reply("err:syntax\r\n");
            return;
        }
        if (nscan == 2) {
            rc = NoteFilter_SetQ(note, q);
            if (rc != 0) {
                RS485_Reply("err:range\r\n");
                return;
            }
        }
        rc = NoteFilter_SetCutoff(note, fc);
        if (rc != 0) {
            RS485_Reply("err:range\r\n");
            return;
        }
        Console_FilterReply(note);
        return;
    }

    if ((line[1] >= '8' && line[1] <= '9') || (line[1] >= 'a' && line[1] <= 'f')) {
        if (line[2] == '\0' || line[2] == ' ') {
            RS485_Reply("err:range\r\n");
            return;
        }
        RS485_Reply("err:syntax\r\n");
        return;
    }

    if (line[1] == '\0' || line[1] == ' ') {
        rest = line + 1;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            /* Compact dump of f0..f7 effective/q/k. */
            int n = snprintf(b, bsz, "ok:");
            for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES && n > 0 && (size_t)n < bsz; i++) {
                fc = NoteFilter_GetCutoff(i);
                n += snprintf(b + n, bsz - (size_t)n, " f%u=%.0f/%.2f/k%.2f", (unsigned)i, fc,
                              NoteFilter_GetQ(i), NoteFilter_GetPitchK(i));
            }
            if ((size_t)n < bsz - 2u) {
                snprintf(b + n, bsz - (size_t)n, "\r\n");
            }
            RS485_Reply(b);
            return;
        }
        nscan = sscanf(rest, "%lf %lf", &fc, &q);
        if (nscan < 1) {
            RS485_Reply("err:syntax\r\n");
            return;
        }
        for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++) {
            if (nscan == 2) {
                rc = NoteFilter_SetQ(i, q);
                if (rc != 0) {
                    RS485_Reply("err:range\r\n");
                    return;
                }
            }
            rc = NoteFilter_SetCutoff(i, fc);
            if (rc != 0) {
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

/* ---- handle pitch tracking commands ------------------------------------- */

/** fk / fk0..fk7: filter pitch-track k (voices 0..7). */
static void Console_CmdFk(char *line, char *b, size_t bsz)
{
    const char *rest;
    double kd;
    int rc;
    uint8_t note;

    if (line[2] == '\0' || line[2] == ' ') {
        rest = line + 2;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            int n = snprintf(b, bsz, "ok:");
            for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES && n > 0 && (size_t)n < bsz; i++) {
                n += snprintf(b + n, bsz - (size_t)n, " fk%u=%.2f", (unsigned)i,
                              NoteFilter_GetPitchK(i));
            }
            if ((size_t)n < bsz - 2u) {
                snprintf(b + n, bsz - (size_t)n, "\r\n");
            }
            RS485_Reply(b);
            return;
        }
        if (sscanf(rest, "%lf", &kd) != 1) {
            RS485_Reply("err:syntax\r\n");
            return;
        }
        for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++) {
            rc = NoteFilter_SetPitchK(i, kd);
            if (rc != 0) {
                RS485_Reply("err:range\r\n");
                return;
            }
        }
        snprintf(b, bsz, "ok: fk %.2f\r\n", kd);
        RS485_Reply(b);
        return;
    }

    if (line[2] >= '0' && line[2] <= '7' && (line[3] == '\0' || line[3] == ' ')) {
        note = (uint8_t)(line[2] - '0');
        rest = line + 3;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            Console_FkReply(note);
            return;
        }
        if (sscanf(rest, "%lf", &kd) != 1) {
            RS485_Reply("err:syntax\r\n");
            return;
        }
        rc = NoteFilter_SetPitchK(note, kd);
        if (rc != 0) {
            RS485_Reply("err:range\r\n");
            return;
        }
        Console_FkReply(note);
        return;
    }

    if ((line[2] >= '8' && line[2] <= '9') || (line[2] >= 'a' && line[2] <= 'f')) {
        if (line[3] == '\0' || line[3] == ' ') {
            RS485_Reply("err:range\r\n");
            return;
        }
    }
    RS485_Reply("err:syntax\r\n");
}

/* ---- handle voice status queries ---------------------------------------- */

/**
 * vq — per-voice playback duration and exact total writable credit.
 */
static void Console_CmdVoiceQuery(void)
{
    char b[384];
    int n;
    uint8_t mask = 0u;
    uint16_t free_samples[NOTE_BANK_VOICES];
    uint8_t pending_mask = 0u;
    uint8_t sessions[NOTE_BANK_VOICES], current_sessions[NOTE_BANK_VOICES];
    uint32_t remaining_us[NOTE_BANK_VOICES];
    uint16_t refill_samples[NOTE_BANK_VOICES];
    uint16_t body_sequence;
    uint8_t body_age_ms;
    uint8_t i;

    {
        uint32_t saved = Board_IrqSave();
        for (i = 0u; i < NOTE_BANK_VOICES; i++) {
            if (NoteBank_IsActive(i) != 0u) mask |= (uint8_t)(1u << i);
            free_samples[i] = (uint16_t)StreamRing_FreeLevel(i);
            sessions[i] = StreamRing_TargetSession(i);
            current_sessions[i] = StreamRing_CurrentSession(i);
            remaining_us[i] = NoteBank_RemainingUs(i);
            refill_samples[i] = NoteBank_RefillSamples5ms(i);
            if (StreamRing_HasPending(i) != 0u)
                pending_mask = (uint8_t)(pending_mask | (uint8_t)(1u << i));
        }
        /* Credit and acknowledgement must describe the same USB state. A newer
         * acknowledgement with older free space grants already-used space again. */
        body_sequence = StreamRing_LastBodySequence();
        body_age_ms = StreamRing_BodyAgeMs();
        Board_IrqRestore(saved);
    }

    if (!console_via_usb) {
        RS485_ReplyVq(mask, pending_mask, sessions, current_sessions, remaining_us, free_samples,
                      body_sequence, body_age_ms, refill_samples);
        return;
    }

    n = snprintf(b, sizeof b, "ok:vq12 %02x %02x %u %u %u", (unsigned)mask, (unsigned)pending_mask,
                 (unsigned)STREAM_RING_SAMPLES, (unsigned)rs485_vq_sequence,
                 (unsigned)body_sequence);
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        if (n < 0 || (size_t)n >= sizeof b) {
            RS485_Reply("err:buf\r\n");
            return;
        }
        n += snprintf(b + n, sizeof b - (size_t)n, " %u %lu %u", (unsigned)sessions[i],
                      (unsigned long)remaining_us[i], (unsigned)free_samples[i]);
        if (n >= 0 && (size_t)n < sizeof b)
            n += snprintf(b + n, sizeof b - (size_t)n, " %u", (unsigned)refill_samples[i]);
    }
    if (n < 0 || (size_t)n >= sizeof b - 2u) {
        RS485_Reply("err:buf\r\n");
        return;
    }
    b[n++] = '\r';
    b[n++] = '\n';
    b[n] = '\0';
    RS485_Reply(b);
}

/* ---- handle note commands ----------------------------------------------- */

/** clear / n off: hard-stop all voices and reset Berry state at the next boundary. */
static void Console_CmdNoteAll(char *line)
{
    if (strcmp(line, "n off") != 0 && strcmp(line, "clear") != 0) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    Console_AllNotesOff();
    RS485_Reply(strcmp(line, "clear") == 0 ? "ok:clear rt=1\r\n" : "ok\r\n");
}

/** n0..n7: note bank on CH1 (also answers unknown; slots 8..f err:range). */
static void Console_CmdNoteSlot(char *line)
{
    unsigned int key;
    unsigned int velocity = 127u;
    unsigned int session;
    char extra;
    uint8_t note;
    int nscan;

    if (line[0] != 'n' || line[1] == '\0') {
        char reply[128];
        (void)snprintf(reply, sizeof(reply), "err:unknown received <%s>\r\n", line);
        RS485_Reply(reply);
        return;
    }

    note = Console_ParseNoteSlot(line[1]);
    if (note == 0xFFu || (line[2] != '\0' && line[2] != ' ')) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    if (note >= NOTE_BANK_VOICES) {
        RS485_Reply("err:range\r\n");
        return;
    }

    /* A slot token without `on` or `off` is not a command. */
    if (line[2] == '\0') {
        RS485_Reply("err:syntax\r\n");
        return;
    }

    if (strcmp(line + 3, "off") == 0) {
        NoteBank_NoteOff(note);
        RS485_Reply("ok\r\n");
        return;
    }
    unsigned int sample;
    nscan = sscanf(line + 3, "on %u %u %u @%u %c", &sample, &key, &velocity, &session, &extra);
    if (nscan == 4) {
        if (sample >= ATTACK_BANK_SAMPLE_COUNT || key >= FW_SCRIPT_CHANNEL_KEY_COUNT ||
            velocity == 0u || velocity > 127u || session >= USB_STREAM_SESSION_MOD) {
            RS485_Reply("err:range\r\n");
            return;
        }
        Console_NoteOn(note, (uint8_t)key, (uint8_t)velocity, (uint16_t)session, (uint16_t)sample);
        return;
    }
    nscan = sscanf(line + 3, "on %u %u @%u %c", &key, &velocity, &session, &extra);
    if (nscan == 3) {
        if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || velocity == 0u || velocity > 127u ||
            session >= USB_STREAM_SESSION_MOD) {
            RS485_Reply("err:range\r\n");
            return;
        }
    } else {
        nscan = sscanf(line + 3, "on %u %u %c", &key, &velocity, &extra);
        if (nscan == 2) {
            if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || velocity == 0u || velocity > 127u) {
                RS485_Reply("err:range\r\n");
                return;
            }
            session = USB_STREAM_SESSION_MOD;
        } else {
            velocity = 127u;
            nscan = sscanf(line + 3, "on %u @%u %c", &key, &session, &extra);
            if (nscan == 2) {
                if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || session >= USB_STREAM_SESSION_MOD) {
                    RS485_Reply("err:range\r\n");
                    return;
                }
            } else {
                nscan = sscanf(line + 3, "on %u %c", &key, &extra);
                if (nscan != 1) {
                    RS485_Reply("err:syntax\r\n");
                    return;
                }
                if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT) {
                    RS485_Reply("err:range\r\n");
                    return;
                }
                session = USB_STREAM_SESSION_MOD;
            }
        }
    }
    Console_NoteOn(note, (uint8_t)key, (uint8_t)velocity, (uint16_t)session, UINT16_MAX);
}

/* ---- handle sample and upload commands ---------------------------------- */

/** a — loaded count + 256-bit hex mask (bit 0 = wave 0). */
static void Console_CmdAttackQuery(void)
{
    char b[96];
    uint8_t mask[32];
    int n;
    uint8_t i;

    AttackBank_LoadedMask(mask);
    n = snprintf(b, sizeof b, "ok: a %u ", (unsigned)AttackBank_LoadedCount());
    for (i = 0u; i < 32u && n > 0 && (size_t)n < sizeof b; i++) {
        n += snprintf(b + n, sizeof b - (size_t)n, "%02x", (unsigned)mask[i]);
    }
    if (n > 0 && (size_t)n < sizeof b) {
        snprintf(b + n, sizeof b - (size_t)n, "\r\n");
    }
    RS485_Reply(b);
}

/** al <wave_id> <nbytes> — CDC signed-int8 load (1..ATTACK_BANK_BYTES). */
static void Console_CmdAttackLoad(char *line)
{
    unsigned int wid;
    unsigned long nbytes;

    if (!console_via_usb) {
        RS485_Reply("err:usb\r\n");
        return;
    }
    if (sscanf(line, "al %u %lu", &wid, &nbytes) != 2) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    if (AttackUpload_Begin((uint16_t)wid, (uint32_t)nbytes) != 0) {
        RS485_Reply("err:range\r\n");
        return;
    }
    RS485_Reply("ok:ready\r\n");
}

/** wl <logical-wave> <nbytes> — card resolves logical 0..7 to bank storage. */
static void Console_CmdWavetableLoad(char *line)
{
    unsigned int wave;
    unsigned long nbytes;
    if (!console_via_usb) {
        RS485_Reply("err:usb\r\n");
        return;
    }
    if (sscanf(line, "wl %u %lu", &wave, &nbytes) != 2 || wave >= ATTACK_BANK_WAVETABLE_COUNT) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    if (AttackUpload_BeginWavetable((uint8_t)wave, (uint32_t)nbytes) != 0) {
        RS485_Reply("err:range\r\n");
        return;
    }
    RS485_Reply("ok:ready\r\n");
}

/** vmload <voice> <nbytes> — receive one Channel ABI2 FWSC container over CDC. */
static void Console_CmdVmLoad(char *line)
{
    unsigned int voice;
    unsigned long nbytes;
    if (!console_via_usb) {
        RS485_Reply("err:usb\r\n");
        return;
    }
    if (sscanf(line, "vmload %u %lu", &voice, &nbytes) != 2 || voice >= FW_VM_CHANNEL_VOICE_COUNT) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    if (NoteBank_AnyActive() != 0u) {
        RS485_Reply("err:vm-busy\r\n");
        return;
    }
    if (AttackUpload_IsActive() != 0u || VmUpload_Begin((uint8_t)voice, (uint32_t)nbytes) != 0) {
        RS485_Reply("err:range\r\n");
        return;
    }
    RS485_Reply("ok:ready\r\n");
}

/* ---- report script status ----------------------------------------------- */

static void Console_CmdVmStatus(char *line)
{
    char reply[192];
    unsigned int voice;
    if (strcmp(line, "vm mem") == 0) {
        const FwVmMemoryMetrics *m = NoteBank_VmMemoryMetrics();
        (void)snprintf(reply, sizeof(reply), "ok:vm mem %lu %lu %lu %lu %lu %lu %lu %lu %u\r\n",
                       (unsigned long)m->arena_size, (unsigned long)m->arena_current,
                       (unsigned long)m->arena_peak, (unsigned long)m->arena_largest_free,
                       (unsigned long)m->load_allocations, (unsigned long)m->handler_allocations,
                       (unsigned long)m->load_gc, (unsigned long)m->handler_gc,
                       (unsigned)m->shared_vm_valid);
        RS485_Reply(reply);
        for (voice = 0u; voice < FW_VM_CHANNEL_VOICE_COUNT; ++voice) {
            (void)snprintf(reply, sizeof(reply), "ok:vm memv %u %u %lu %lu\r\n", voice,
                           (unsigned)NoteBank_VmFault((uint8_t)voice),
                           (unsigned long)NoteBank_VmMaxCycles((uint8_t)voice),
                           (unsigned long)NoteBank_VmFaultCount((uint8_t)voice));
            RS485_Reply(reply);
        }
        return;
    } else if (strcmp(line, "vm") == 0) {
        (void)snprintf(reply, sizeof(reply), "ok:vm mask %02x\r\n",
                       (unsigned)NoteBank_VmActiveMask());
    } else if (sscanf(line, "vm %u", &voice) != 1 || voice >= FW_VM_CHANNEL_VOICE_COUNT) {
        (void)snprintf(reply, sizeof(reply), "err:syntax\r\n");
    } else if (NoteBank_VmIsActive((uint8_t)voice) != 0u) {
        (void)snprintf(reply, sizeof(reply), "ok:vm %u active %lu %u %u\r\n", voice,
                       (unsigned long)FW_VM_TARGET_CHANNEL, (unsigned)FW_VM_TARGET_CHANNEL_VERSION,
                       (unsigned)NoteBank_VmFault((uint8_t)voice));
    } else {
        (void)snprintf(reply, sizeof(reply), "ok:vm %u inactive %u\r\n", voice,
                       (unsigned)NoteBank_VmFault((uint8_t)voice));
    }
    RS485_Reply(reply);
}

/* ---- set sample root pitch ---------------------------------------------- */

/** ar <wave_id> <root_hz> — attack-head native pitch for on-card rate-scale. */
static void Console_CmdRoot(char *line)
{
    unsigned int wid;
    float hz;

    if (sscanf(line, "ar %u %f", &wid, &hz) != 2) {
        RS485_Reply("err:syntax\r\n");
        return;
    }
    if (wid >= ATTACK_BANK_COUNT || !(hz > 0.0f)) {
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

/* ---- dispatch console commands ------------------------------------------ */

static void Console_Exec(char *line)
{
    char b[160];
    unsigned int ch, val;

    if (line[0] == '\0') {
        return;
    }

    /* Clear receive state before replying so the acknowledgement survives. */
    if (strcmp(line, "reset") == 0) {
        Uart5Rx_Clear();
        rs485_line_len = 0u;
        rs485_line[0] = '\0';
        RS485_Reply("ok:reset\r\n");
        return;
    }

    /* ---- h / help / ? ------------------------------------------------------- */

    if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        Console_Help();
        return;
    }

    if (strncmp(line, "vmload ", 7) == 0) {
        Console_CmdVmLoad(line);
        return;
    }

    if (strcmp(line, "vm") == 0 || strncmp(line, "vm ", 3) == 0) {
        Console_CmdVmStatus(line);
        return;
    }

    if (strcmp(line, "cpuload") == 0) {
        (void)snprintf(b, sizeof b, "ok: cpuload %u\r\n", (unsigned)Audio_Bridge_CpuLoadProbeGet());
        RS485_Reply(b);
        return;
    }

    if (strncmp(line, "cpuload ", 8) == 0) {
        unsigned int enabled;
        char extra;
        if (sscanf(line + 8, "%u %c", &enabled, &extra) != 1 || enabled > 1u) {
            RS485_Reply("err:syntax\r\n");
            return;
        }
        Audio_Bridge_CpuLoadProbeSet((uint8_t)enabled);
        led_show_on = enabled == 0u ? 1u : 0u;
        (void)snprintf(b, sizeof b, "ok: cpuload %u\r\n", enabled);
        RS485_Reply(b);
        return;
    }

    /* ---- al <id> nbytes: cdc attack head upload ----------------------------- */

    if (strncmp(line, "al ", 3) == 0) {
        Console_CmdAttackLoad(line);
        return;
    }

    /* ---- wl <logical-wave> nbytes: card-placed oscillator wavetable --------- */

    if (strncmp(line, "wl ", 3) == 0) {
        Console_CmdWavetableLoad(line);
        return;
    }

    /* ---- ar <id> <hz>: sample root pitch ------------------------------------ */

    if (strncmp(line, "ar ", 3) == 0) {
        Console_CmdRoot(line);
        return;
    }

    /* ---- a: loaded heads (slot = voice) ------------------------------------- */

    if (strcmp(line, "a") == 0) {
        Console_CmdAttackQuery();
        return;
    }

    /* ---- vq: active-voice mask + exact per-voice free samples --------------- */

    if (strcmp(line, "vq") == 0) {
        Console_CmdVoiceQuery();
        return;
    }

    if (strcmp(line, "rs485") == 0) {
        char debug[128];
        (void)snprintf(debug, sizeof debug, "ok: rs485 rxdrop %lu txfail %lu trunc %lu\r\n",
                       (unsigned long)Uart5Rx_DroppedCount(), (unsigned long)rs485_tx_fail,
                       (unsigned long)rs485_tx_trunc);
        RS485_Reply(debug);
        return;
    }
    if (strcmp(line, "usbdev") == 0) {
        char debug[300];
        USB_Device_Debug(debug, sizeof debug);
        RS485_Reply(debug);
        return;
    }
    /* ---- usb [0]: body counters (fifo drop, hold, fill) --------------------- */

    if (strncmp(line, "usb", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        if (line[3] == ' ') {
            unsigned z = 1u;
            if (sscanf(line + 4, "%u", &z) != 1 || z != 0u) {
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

            if (minf == 0xFFFFFFFFu) {
                min_s[0] = '-';
                min_s[1] = '\0';
            } else {
                snprintf(min_s, sizeof min_s, "%lu", (unsigned long)minf);
            }
            snprintf(
                usb_b, sizeof usb_b,
                "ok: usb drop %lu hold %lu min %s fill %lu "
                "z %lu sof %lu vq %lu win %lu rx %lu bytes %lu "
                "bad %lu h%lu s%lu f%lu c%lu u%lu late %lu "
                "stale %lu future %lu full %lu superseded %lu\r\n",
                (unsigned long)Audio_Bridge_UsbDropCount(), (unsigned long)NoteBank_HoldCount(),
                min_s, (unsigned long)Audio_Bridge_MaxFill(), (unsigned long)StreamRing_ZeroCount(),
                (unsigned long)StreamRing_SofCount(), (unsigned long)rs485_vq_count,
                (unsigned long)USB_App_BlockCount(), (unsigned long)USB_App_RxMsgCount(),
                (unsigned long)USB_App_RxByteCount(), (unsigned long)USB_App_BadCount(),
                (unsigned long)USB_App_BadReasonCount(0u),
                (unsigned long)USB_App_BadReasonCount(1u),
                (unsigned long)USB_App_BadReasonCount(2u),
                (unsigned long)USB_App_BadReasonCount(3u),
                (unsigned long)USB_App_BadReasonCount(4u), (unsigned long)Audio_Bridge_FillLate(),
                (unsigned long)StreamRing_StaleCount(), (unsigned long)StreamRing_FutureCount(),
                (unsigned long)StreamRing_FullCount(), (unsigned long)StreamRing_SupersededCount());
            RS485_Reply(usb_b);
        }
        return;
    }

    /* ---- g <ch> <db>: cs4304 dac atten -------------------------------------- */

    if (sscanf(line, "g %u %u", &ch, &val) == 2) {
        (void)ch;
        (void)val;
        Console_CmdGain(line);
        return;
    }

    /* ---- fk / fk0..fk7: filter pitch-track (before bare f) ------------------ */

    if (line[0] == 'f' && line[1] == 'k') {
        Console_CmdFk(line, b, sizeof b);
        return;
    }

    /* ---- f0..f7 / f: lpf on first 8 voices; optional q = df4 g -------------- */

    if (line[0] == 'f') {
        Console_CmdFilter(line, b, sizeof b);
        return;
    }

    if (strcmp(line, "clear") == 0) {
        Console_CmdNoteAll(line);
        return;
    }

    if (line[0] == 'n') {
        console_note_event = 1u;
        if (line[1] == '\0' || line[1] == ' ')
            Console_CmdNoteAll(line);
        else
            Console_CmdNoteSlot(line);
        console_note_event = 0u;
        return;
    }

    Console_CmdNoteSlot(line);
}

/* ---- resolve rs485 addressing ------------------------------------------- */

/** Check if a received line is addressed to this card.
 * Prefix format:  "X:command"  where X is a card ID letter.
 *   'C' = Channel Card (us),  'E' = Effect Card,  '*' = broadcast.
 * No prefix = broadcast.
 *
 * If addressed to us, strips the prefix and returns 1.
 * If addressed to another card, returns 0 (ignore).
 * The stripped command is written back to `line`. */
static uint8_t RS485_IsForMe(char *line)
{
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
    /* No prefix → broadcast. */
    return 1;
}

/* ---- retain the internal usb command entry point ------------------------ */

/* Legacy internal entry point. The binary USB receiver does not dispatch
 * text through this function. */
void Console_ExecFromUSB(char *line)
{
    if (line == NULL) {
        return;
    }
    if (!RS485_IsForMe(line)) /* accept "c:", "*:" or bare commands */
        return;
    console_via_usb = 1;
    Console_Exec(line);
    console_via_usb = 0;
}

/* ---- receive rs485 command lines ---------------------------------------- */

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * Messages addressed to other cards are ignored. */
static void Console_Poll(void)
{
    uint8_t c;

    while (Uart5Rx_Get(&c)) {
        if (c == '\r' || c == '\n') {
#if RS485_ECHO
            RS485_Send("\r\n");
#endif
            rs485_line[rs485_line_len] = '\0';

            /* Execute only commands addressed to this card or broadcast. */
            if (RS485_IsForMe((char *)rs485_line)) {
                rs485_cmd_count++;
                Console_Exec((char *)rs485_line);
            }
            /* else: message for another card — silently ignore */

            rs485_line_len = 0;
        } else if (c == 0x08 || c == 0x7F) /* backspace */
        {
            if (rs485_line_len > 0) {
                rs485_line_len--;
#if RS485_ECHO
                RS485_Send("\b \b");
#endif
            }
        } else if (c >= 32 && c < 127 && rs485_line_len < sizeof(rs485_line) - 1) {
            rs485_line[rs485_line_len++] = (uint8_t)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            /* A fresh address starts a new command prefix, discarding any
             * preceding bus echo or partial address. */
            if (rs485_line_len >= 2u && rs485_line[rs485_line_len - 1u] == ':' &&
                (rs485_line[rs485_line_len - 2u] == (uint8_t)RS485_CARD_ID ||
                 rs485_line[rs485_line_len - 2u] == (uint8_t)RS485_BROADCAST_ID ||
                 rs485_line[rs485_line_len - 2u] == (uint8_t)'e')) {
                rs485_line[0] = rs485_line[rs485_line_len - 2u];
                rs485_line[1] = ':';
                rs485_line_len = 2u;
            }
#if RS485_ECHO
            char e[2] = {(char)c, '\0'};
            RS485_Send(e); /* echo typing */
#endif
        }
    }
}

/* ---- update status leds ------------------------------------------------- */

/** Non-blocking fixed red/yellow chaser (150 ms per step).
 * The RGB package is exclusively VM-controlled. */
static void LED_Task(void)
{
    static const uint8_t leds[2] = {BOARD_LED_R, BOARD_LED_Y};
    static uint32_t t_next = 0;
    static uint8_t step = 0;

    ChannelLed_Task();
    if (Audio_Bridge_CpuLoadProbeGet() != 0u) {
        Board_LedWrite(BOARD_LED_R, 0u);
        return;
    }
    if (!led_show_on) {
        for (uint8_t i = 0; i < 2; i++) {
            Board_LedWrite(leds[i], 0u);
        }
        return;
    }
    if (Board_Millis() >= t_next) {
        t_next = Board_Millis() + 150;
        Board_LedWrite(leds[step], 0u);
        step = (step + 1) % 2;
        Board_LedWrite(leds[step], 1u);
    }
}

/* ---- initialize and poll the console ------------------------------------ */

void ChannelConsole_Init(void)
{
    ChannelLed_Init();
    Board_Rs485Release();
    Uart5Rx_Init();

    for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
        Board_SwitchWrite(switches[i].id, switches[i].active_low ? 1u : 0u);
    }
    Console_ApplyBootDefaults();

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

/* ---- run one main-loop pass --------------------------------------------- */

void ChannelApp_Step(void)
{
    USB_App_Task();        /* Binary CDC BODY/uploads */
    ChannelConsole_Poll(); /* RS485 console + LED chaser */
    USB_App_Task();        /* Drain CDC after a console TX */
}

/* ---- receive compiled voice programs ------------------------------------ */

static uint8_t s_active;
static uint8_t s_voice;
static uint32_t s_expected;
static uint32_t s_received;

/* ---- query and abort a script upload ------------------------------------ */

uint8_t VmUpload_IsActive(void)
{
    return s_active;
}

void VmUpload_Abort(void)
{
    if (s_active != 0u) NoteBank_VmUploadAbort(s_voice);
    s_active = 0u;
    s_expected = 0u;
    s_received = 0u;
}

/* ---- begin a script upload ---------------------------------------------- */

int VmUpload_Begin(uint8_t voice, uint32_t nbytes)
{
    if (voice >= FW_VM_CHANNEL_VOICE_COUNT || s_active != 0u ||
        nbytes < FW_SCRIPT_CONTAINER_HEADER_SIZE ||
        nbytes > FW_SCRIPT_CONTAINER_HEADER_SIZE + FW_SCRIPT_MAX_PAYLOAD ||
        NoteBank_VmUploadBegin(voice) != 0)
        return -1;
    s_voice = voice;
    s_expected = nbytes;
    s_received = 0u;
    s_active = 1u;
    return 0;
}

/* ---- receive and commit a script upload --------------------------------- */

uint32_t VmUpload_Feed(const uint8_t *data, uint32_t size)
{
    uint32_t take;
    if (s_active == 0u || data == NULL) return 0u;
    take = size;
    if (take > s_expected - s_received) take = s_expected - s_received;
    if (NoteBank_VmUploadFeed(s_voice, data, take) != 0) {
        USB_CDC_WriteStr("err:vm-container\r\n");
        VmUpload_Abort();
        return take;
    }
    s_received += take;
    if (s_received == s_expected) {
        char reply[64];
        if (NoteBank_VmUploadCommit(s_voice) != 0) {
            (void)snprintf(reply, sizeof(reply), "err:vm %u %u\r\n", (unsigned)s_voice,
                           (unsigned)NoteBank_VmFault(s_voice));
        } else {
            (void)snprintf(reply, sizeof(reply), "ok:vm %u %lu %u\r\n", (unsigned)s_voice,
                           (unsigned long)FW_VM_TARGET_CHANNEL,
                           (unsigned)FW_VM_TARGET_CHANNEL_VERSION);
        }
        USB_CDC_WriteStr(reply);
        s_active = 0u;
        s_expected = 0u;
        s_received = 0u;
    }
    return take;
}
