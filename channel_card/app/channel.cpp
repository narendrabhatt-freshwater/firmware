/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#include "transport.h"
#include "channel.h"
#include "audio.h"
#include "platform.h"
#include "voice.h"
#include "filter.h"
#include "samples.h"
#include "stream.h"
#include "usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- request a logical rgb colour --------------------------------------- */

static const ChannelOutputs *s_outputs;

void set_channel_outputs(const ChannelOutputs *outputs)
{
    s_outputs = outputs;
}

int set_channel_led_colour(float red, float green, float blue, float brightness)
{
    return s_outputs && s_outputs->set_led_colour
        ? s_outputs->set_led_colour(red, green, blue, brightness) : 0;
}

/* ---- process console commands ------------------------------------------- */

/* Channel Card RS485 console.
 * main.c owns bring-up and the DAC handle; this file owns the interactive
 * console surface. */

/* ---- multi-drop rs485 card addressing ----------------------------------- */

#define RS485_CARD_ID 'c'
#define RS485_BROADCAST_ID '*'
#define RS485_ECHO 0

#define RS485_TAG "[C] " /* response prefix tag for Channel Card */

/** Transmit a string on the RS485 bus with collision avoidance.
 * Waits for the bus to be free, asserts DE, sends data, releases DE.
 * Returns 0 on success, -1 if the bus was busy (timeout). */
static int __attribute__((unused)) send_rs485_text(const char *s)
{
    if (!wait_for_rs485_bus(get_transport_timeouts()->rs485_bus_ms)) return -1; /* bus occupied — drop this message */

    {
        uint32_t n = (uint32_t)strlen(s);
        write_rs485_bytes((const uint8_t *)s, (uint16_t)n, get_transport_timeouts()->rs485_transmit_ms);
    }

    return 0;
}

/* Main-loop RS485 parser state. USB uses its own binary parser. */
static uint8_t rs485_line[96];
static uint8_t rs485_line_len;

static uint8_t console_note_event;

/* RS485 path counters (internal; no console cmd). */
static uint32_t rs485_tx_fail;   /**< RS485 transmission failed or timed out */
static uint32_t rs485_tx_trunc;  /**< reply longer than the frame buffer */
static uint32_t rs485_vq_count;  /**< exact refill-status frames transmitted */

/* ---- send tagged console replies ---------------------------------------- */

/** Send a tagged response: prefixes the string with [C] so the host knows
 * which card replied.
 *
 * Frame must hold the longest reply (~110 chars) — a short buffer silently
 * swallowed those ACKs and looked like a dead bus. Truncate rather than
 * drop: a clipped line still terminates the host's exchange. */
static void send_rs485_reply(const char *s)
{
    if (console_note_event) return;
    char frame[224];
    size_t tag_len;
    size_t body_len;
    size_t max_body;

    tag_len = strlen(RS485_TAG);
    body_len = strlen(s);
    max_body = sizeof(frame) - tag_len - 1u;
    if (body_len > max_body) {
        body_len = max_body;
        rs485_tx_trunc++;
    }
    memcpy(frame, RS485_TAG, tag_len);
    memcpy(frame + tag_len, s, body_len);

    if (write_rs485_bytes((const uint8_t *)frame, (uint16_t)(tag_len + body_len),
                         get_transport_timeouts()->rs485_transmit_ms) != 0) {
        rs485_tx_fail++;
    }
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

static void send_rs485_voice_status(uint8_t active_mask, uint8_t pending_mask,
                                    const uint8_t *sessions, const uint8_t *current_sessions,
                                    const uint32_t *remaining_us, const uint16_t *free_samples,
                                    uint16_t body_sequence, uint8_t body_age_ms,
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

    if (write_rs485_bytes(frame, VQ_FRAME_LEN, get_transport_timeouts()->rs485_transmit_ms) != 0) {
        rs485_tx_fail++;
    } else {
        rs485_vq_count++;
    }
}

/* ---- channel card control console (rs485) ------------------------------- */

/* RS485 commands are parsed below. Uploads use the binary USB protocol. */
/** Console-addressable filter slots. */
#define NOTE_FILTER_CONSOLE_VOICES 8u

/* ---- parse voice selection ---------------------------------------------- */

/** Map hex digit '0'..'9','a'..'f' → 0..15; else 0xFF. */
static uint8_t parse_voice_index(char hex_digit)
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
static void queue_console_note(uint8_t note, uint8_t key, uint8_t velocity, uint16_t session,
                               uint16_t sample)
{
    if (is_voice_script_upload_busy() != 0u) {
        send_rs485_reply("err:vm-busy\r\n");
        return;
    }
    if (is_voice_script_active(note) == 0u) {
        send_rs485_reply("err:no-program\r\n");
        return;
    }

    int result;
    if (sample != UINT16_MAX)
        result = queue_streamed_sample_note(note, sample, key, velocity, (uint8_t)session);
    else if (session < USB_STREAM_SESSION_MOD)
        result = queue_streamed_voice_note(note, key, velocity, (uint8_t)session);
    else
        result = queue_voice_note(note, key, velocity);
    send_rs485_reply(result == 0    ? "ok\r\n"
                     : result == -3 ? "err:busy\r\n"
                     : result == -2 ? "err:no-program\r\n"
                               : "err:range\r\n");
}

/** Hard-stop every voice and reset Berry state without unloading programs. */
static void request_console_voice_stop(void)
{
    request_stop_all_voices();
}

/* ---- describe console commands ------------------------------------------ */

static void send_command_help(void)
{
    char b[320];
    /* One tagged line — leading \\r\\n would make the host see bare "[C]". */
    snprintf(b, sizeof b,
             "ok: reset (RS485 RX) | SAMPLE n0..n7 on sample key velocity @session | on key "
             "[velocity] [@session] | off | "
             "clear | n off | al id n | wl wave n | vmload v n | vm [v] | ar id Hz | a | vq | "
             "usb | "
             "f0..f7 Hz [q] | fk0..fk7 k | g ch dB\r\n");
    send_rs485_reply(b);
}

/* ---- report filter settings --------------------------------------------- */

static void send_voice_filter_settings(uint8_t voice)
{
    char b[128];
    double base = get_voice_filter_base_cutoff_hz(voice);
    double fc = get_voice_filter_cutoff_hz(voice);
    double k = get_voice_filter_pitch_tracking(voice);

    if (fc >= NOTE_FILTER_CUTOFF_MAX_HZ) {
        snprintf(b, sizeof b, "ok: f%u %.1f Hz (bypass) k %.2f q %.2f\r\n", (unsigned)voice, base,
                 k, get_voice_filter_resonance(voice));
    } else if (k > 0.0) {
        snprintf(b, sizeof b, "ok: f%u base %.1f → %.1f Hz k %.2f q %.2f\r\n", (unsigned)voice,
                 base, fc, k, get_voice_filter_resonance(voice));
    } else {
        snprintf(b, sizeof b, "ok: f%u %.1f Hz k %.2f q %.2f\r\n", (unsigned)voice, fc, k,
                 get_voice_filter_resonance(voice));
    }
    send_rs485_reply(b);
}

static void send_voice_pitch_tracking_settings(uint8_t voice)
{
    char b[48];
    snprintf(b, sizeof b, "ok: fk%u %.2f\r\n", (unsigned)voice,
             get_voice_filter_pitch_tracking(voice));
    send_rs485_reply(b);
}

/* ---- handle dac gain commands ------------------------------------------- */

/** g <ch> <dB>: output attenuation. Caller already matched sscanf. */
static void process_gain_command(char *line)
{
    unsigned int ch, val;

    (void)sscanf(line, "g %u %u", &ch, &val);
    if (ch >= 1 && ch <= 4 && val <= 127 &&
        (!s_outputs || !s_outputs->set_gain ||
         s_outputs->set_gain((char)('0' + ch), (uint8_t)(val * 2u)) == 0)) {
        send_rs485_reply("ok\r\n");
    } else {
        send_rs485_reply("err:range\r\n");
    }
}

/* ---- handle filter commands --------------------------------------------- */

/** f0..f7 / f: LPF on first 8 voices; optional q = DF4 g. */
static void process_filter_command(char *line, char *b, size_t bsz)
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
            send_voice_filter_settings(note);
            return;
        }
        nscan = sscanf(rest, "%lf %lf", &fc, &q);
        if (nscan < 1) {
            send_rs485_reply("err:syntax\r\n");
            return;
        }
        if (nscan == 2) {
            rc = set_voice_filter_resonance(note, q);
            if (rc != 0) {
                send_rs485_reply("err:range\r\n");
                return;
            }
        }
        rc = set_voice_filter_cutoff_hz(note, fc);
        if (rc != 0) {
            send_rs485_reply("err:range\r\n");
            return;
        }
        send_voice_filter_settings(note);
        return;
    }

    if ((line[1] >= '8' && line[1] <= '9') || (line[1] >= 'a' && line[1] <= 'f')) {
        if (line[2] == '\0' || line[2] == ' ') {
            send_rs485_reply("err:range\r\n");
            return;
        }
        send_rs485_reply("err:syntax\r\n");
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
                fc = get_voice_filter_cutoff_hz(i);
                n += snprintf(b + n, bsz - (size_t)n, " f%u=%.0f/%.2f/k%.2f", (unsigned)i, fc,
                              get_voice_filter_resonance(i), get_voice_filter_pitch_tracking(i));
            }
            if ((size_t)n < bsz - 2u) {
                snprintf(b + n, bsz - (size_t)n, "\r\n");
            }
            send_rs485_reply(b);
            return;
        }
        nscan = sscanf(rest, "%lf %lf", &fc, &q);
        if (nscan < 1) {
            send_rs485_reply("err:syntax\r\n");
            return;
        }
        for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++) {
            if (nscan == 2) {
                rc = set_voice_filter_resonance(i, q);
                if (rc != 0) {
                    send_rs485_reply("err:range\r\n");
                    return;
                }
            }
            rc = set_voice_filter_cutoff_hz(i, fc);
            if (rc != 0) {
                send_rs485_reply("err:range\r\n");
                return;
            }
        }
        snprintf(b, bsz, "ok: f %.1f Hz%s k %.2f q %.2f\r\n", fc,
                 (fc == 0.0 || fc >= NOTE_FILTER_CUTOFF_MAX_HZ) ? " (bypass)" : "",
                 get_voice_filter_pitch_tracking(0), get_voice_filter_resonance(0));
        send_rs485_reply(b);
        return;
    }

    send_rs485_reply("err:unknown\r\n");
}

/* ---- handle pitch tracking commands ------------------------------------- */

/** fk / fk0..fk7: filter pitch-track k (voices 0..7). */
static void process_pitch_tracking_command(char *line, char *b, size_t bsz)
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
                              get_voice_filter_pitch_tracking(i));
            }
            if ((size_t)n < bsz - 2u) {
                snprintf(b + n, bsz - (size_t)n, "\r\n");
            }
            send_rs485_reply(b);
            return;
        }
        if (sscanf(rest, "%lf", &kd) != 1) {
            send_rs485_reply("err:syntax\r\n");
            return;
        }
        for (uint8_t i = 0; i < NOTE_FILTER_CONSOLE_VOICES; i++) {
            rc = set_voice_filter_pitch_tracking(i, kd);
            if (rc != 0) {
                send_rs485_reply("err:range\r\n");
                return;
            }
        }
        snprintf(b, bsz, "ok: fk %.2f\r\n", kd);
        send_rs485_reply(b);
        return;
    }

    if (line[2] >= '0' && line[2] <= '7' && (line[3] == '\0' || line[3] == ' ')) {
        note = (uint8_t)(line[2] - '0');
        rest = line + 3;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            send_voice_pitch_tracking_settings(note);
            return;
        }
        if (sscanf(rest, "%lf", &kd) != 1) {
            send_rs485_reply("err:syntax\r\n");
            return;
        }
        rc = set_voice_filter_pitch_tracking(note, kd);
        if (rc != 0) {
            send_rs485_reply("err:range\r\n");
            return;
        }
        send_voice_pitch_tracking_settings(note);
        return;
    }

    if ((line[2] >= '8' && line[2] <= '9') || (line[2] >= 'a' && line[2] <= 'f')) {
        if (line[3] == '\0' || line[3] == ' ') {
            send_rs485_reply("err:range\r\n");
            return;
        }
    }
    send_rs485_reply("err:syntax\r\n");
}

/* ---- handle voice status queries ---------------------------------------- */

/**
 * vq — per-voice playback duration and exact total writable credit.
 */
static void process_voice_status_command(void)
{
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
        uint32_t saved = disable_interrupts_and_save_state();
        for (i = 0u; i < NOTE_BANK_VOICES; i++) {
            uint8_t pending;
            get_stream_sessions(i, &current_sessions[i], &sessions[i], &pending);
            /* Streaming activity and session identity must use the same state.
             * The renderer snapshot can lag both promotion and retirement. */
            if (current_sessions[i] != 255u)
                mask |= (uint8_t)(1u << i);
            free_samples[i] = (uint16_t)get_stream_free_samples(i);
            remaining_us[i] = get_voice_buffered_time_us(i);
            refill_samples[i] = get_voice_refill_samples_5ms(i);
            if (pending != 0u)
                pending_mask = (uint8_t)(pending_mask | (uint8_t)(1u << i));
        }
        /* Credit and acknowledgement must describe the same USB state. A newer
         * acknowledgement with older free space grants already-used space again. */
        body_sequence = get_last_stream_body_sequence();
        body_age_ms = get_stream_body_age_ms();
        restore_interrupt_state(saved);
    }

    send_rs485_voice_status(mask, pending_mask, sessions, current_sessions, remaining_us,
                           free_samples, body_sequence, body_age_ms, refill_samples);
}

/* ---- handle note commands ----------------------------------------------- */

/** clear / n off: hard-stop all voices and reset Berry state at the next boundary. */
static void process_all_voices_command(char *line)
{
    if (strcmp(line, "n off") != 0 && strcmp(line, "clear") != 0) {
        send_rs485_reply("err:syntax\r\n");
        return;
    }
    request_console_voice_stop();
    send_rs485_reply(strcmp(line, "clear") == 0 ? "ok:clear rt=1\r\n" : "ok\r\n");
}

/** n0..n7: note bank on CH1 (also answers unknown; slots 8..f err:range). */
static void process_voice_note_command(char *line)
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
        send_rs485_reply(reply);
        return;
    }

    note = parse_voice_index(line[1]);
    if (note == 0xFFu || (line[2] != '\0' && line[2] != ' ')) {
        send_rs485_reply("err:syntax\r\n");
        return;
    }
    if (note >= NOTE_BANK_VOICES) {
        send_rs485_reply("err:range\r\n");
        return;
    }

    /* A slot token without `on` or `off` is not a command. */
    if (line[2] == '\0') {
        send_rs485_reply("err:syntax\r\n");
        return;
    }

    if (strcmp(line + 3, "off") == 0) {
        queue_voice_note_off(note);
        send_rs485_reply("ok\r\n");
        return;
    }
    unsigned int sample;
    nscan = sscanf(line + 3, "on %u %u %u @%u %c", &sample, &key, &velocity, &session, &extra);
    if (nscan == 4) {
        if (sample >= ATTACK_BANK_SAMPLE_COUNT || key >= FW_SCRIPT_CHANNEL_KEY_COUNT ||
            velocity == 0u || velocity > 127u || session >= USB_STREAM_SESSION_MOD) {
            send_rs485_reply("err:range\r\n");
            return;
        }
        queue_console_note(note, (uint8_t)key, (uint8_t)velocity, (uint16_t)session,
                           (uint16_t)sample);
        return;
    }
    nscan = sscanf(line + 3, "on %u %u @%u %c", &key, &velocity, &session, &extra);
    if (nscan == 3) {
        if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || velocity == 0u || velocity > 127u ||
            session >= USB_STREAM_SESSION_MOD) {
            send_rs485_reply("err:range\r\n");
            return;
        }
    } else {
        nscan = sscanf(line + 3, "on %u %u %c", &key, &velocity, &extra);
        if (nscan == 2) {
            if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || velocity == 0u || velocity > 127u) {
                send_rs485_reply("err:range\r\n");
                return;
            }
            session = USB_STREAM_SESSION_MOD;
        } else {
            velocity = 127u;
            nscan = sscanf(line + 3, "on %u @%u %c", &key, &session, &extra);
            if (nscan == 2) {
                if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT || session >= USB_STREAM_SESSION_MOD) {
                    send_rs485_reply("err:range\r\n");
                    return;
                }
            } else {
                nscan = sscanf(line + 3, "on %u %c", &key, &extra);
                if (nscan != 1) {
                    send_rs485_reply("err:syntax\r\n");
                    return;
                }
                if (key >= FW_SCRIPT_CHANNEL_KEY_COUNT) {
                    send_rs485_reply("err:range\r\n");
                    return;
                }
                session = USB_STREAM_SESSION_MOD;
            }
        }
    }
    queue_console_note(note, (uint8_t)key, (uint8_t)velocity, (uint16_t)session, UINT16_MAX);
}

/* ---- handle sample and upload commands ---------------------------------- */

/** a — loaded count + 256-bit hex mask (bit 0 = wave 0). */
static void process_sample_status_command(void)
{
    char b[96];
    uint8_t mask[32];
    int n;
    uint8_t i;

    get_loaded_sample_mask(mask);
    n = snprintf(b, sizeof b, "ok: a %u ", (unsigned)get_loaded_sample_count());
    for (i = 0u; i < 32u && n > 0 && (size_t)n < sizeof b; i++) {
        n += snprintf(b + n, sizeof b - (size_t)n, "%02x", (unsigned)mask[i]);
    }
    if (n > 0 && (size_t)n < sizeof b) {
        snprintf(b + n, sizeof b - (size_t)n, "\r\n");
    }
    send_rs485_reply(b);
}

/* ---- report script status ----------------------------------------------- */

static void process_script_status_command(char *line)
{
    char reply[192];
    unsigned int voice;
    if (strcmp(line, "vm mem") == 0) {
        const FwVmMemoryMetrics *m = get_voice_script_memory_metrics();
        (void)snprintf(reply, sizeof(reply), "ok:vm mem %lu %lu %lu %lu %lu %lu %lu %lu %u\r\n",
                       (unsigned long)m->arena_size, (unsigned long)m->arena_current,
                       (unsigned long)m->arena_peak, (unsigned long)m->arena_largest_free,
                       (unsigned long)m->load_allocations, (unsigned long)m->handler_allocations,
                       (unsigned long)m->load_gc, (unsigned long)m->handler_gc,
                       (unsigned)m->shared_vm_valid);
        send_rs485_reply(reply);
        for (voice = 0u; voice < FW_VM_CHANNEL_VOICE_COUNT; ++voice) {
            (void)snprintf(reply, sizeof(reply), "ok:vm memv %u %u %lu %lu\r\n", voice,
                           (unsigned)get_voice_script_fault((uint8_t)voice),
                           (unsigned long)get_voice_script_max_cycles((uint8_t)voice),
                           (unsigned long)get_voice_script_fault_count((uint8_t)voice));
            send_rs485_reply(reply);
        }
        return;
    } else if (strcmp(line, "vm") == 0) {
        (void)snprintf(reply, sizeof(reply), "ok:vm mask %02x\r\n",
                       (unsigned)get_active_voice_script_mask());
    } else if (sscanf(line, "vm %u", &voice) != 1 || voice >= FW_VM_CHANNEL_VOICE_COUNT) {
        (void)snprintf(reply, sizeof(reply), "err:syntax\r\n");
    } else if (is_voice_script_active((uint8_t)voice) != 0u) {
        (void)snprintf(reply, sizeof(reply), "ok:vm %u active %lu %u %u\r\n", voice,
                       (unsigned long)FW_VM_TARGET_CHANNEL, (unsigned)FW_VM_TARGET_CHANNEL_VERSION,
                       (unsigned)get_voice_script_fault((uint8_t)voice));
    } else {
        (void)snprintf(reply, sizeof(reply), "ok:vm %u inactive %u\r\n", voice,
                       (unsigned)get_voice_script_fault((uint8_t)voice));
    }
    send_rs485_reply(reply);
}

/* ---- set sample root pitch ---------------------------------------------- */

/** ar <wave_id> <root_hz> — attack-head native pitch for on-card rate-scale. */
static void process_sample_root_frequency_command(char *line)
{
    unsigned int wid;
    float hz;

    if (sscanf(line, "ar %u %f", &wid, &hz) != 2) {
        send_rs485_reply("err:syntax\r\n");
        return;
    }
    if (wid >= ATTACK_BANK_COUNT || !(hz > 0.0f)) {
        send_rs485_reply("err:range\r\n");
        return;
    }
    set_sample_root_frequency_hz((uint16_t)wid, hz);
    {
        char b[48];
        snprintf(b, sizeof b, "ok: ar %u %.6g\r\n", wid, (double)hz);
        send_rs485_reply(b);
    }
}

/* ---- dispatch console commands ------------------------------------------ */

static void execute_channel_command(char *line)
{
    char b[160];
    unsigned int ch, val;

    if (line[0] == '\0') {
        return;
    }

    /* Clear receive state before replying so the acknowledgement survives. */
    if (strcmp(line, "reset") == 0) {
        clear_rs485_receive_state();
        rs485_line_len = 0u;
        rs485_line[0] = '\0';
        send_rs485_reply("ok:reset\r\n");
        return;
    }

    /* ---- h / help / ? --------------------------------------------------- */

    if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        send_command_help();
        return;
    }

    // Uploads are binary USB requests. Preserve the RS485 error for old clients.
    if (strncmp(line, "vmload ", 7) == 0 || strncmp(line, "al ", 3) == 0 ||
        strncmp(line, "wl ", 3) == 0) {
        send_rs485_reply("err:usb\r\n");
        return;
    }

    if (strcmp(line, "vm") == 0 || strncmp(line, "vm ", 3) == 0) {
        process_script_status_command(line);
        return;
    }

    /* ---- ar <id> <hz>: sample root pitch -------------------------------- */

    if (strncmp(line, "ar ", 3) == 0) {
        process_sample_root_frequency_command(line);
        return;
    }

    /* ---- a: loaded heads (slot = voice) --------------------------------- */

    if (strcmp(line, "a") == 0) {
        process_sample_status_command();
        return;
    }

    /* ---- vq: active-voice mask + exact per-voice free samples ----------- */

    if (strcmp(line, "vq") == 0) {
        process_voice_status_command();
        return;
    }

    if (strcmp(line, "rs485") == 0) {
        char debug[128];
        (void)snprintf(debug, sizeof debug, "ok: rs485 rxdrop %lu txfail %lu trunc %lu\r\n",
                       (unsigned long)get_rs485_dropped_byte_count(), (unsigned long)rs485_tx_fail,
                       (unsigned long)rs485_tx_trunc);
        send_rs485_reply(debug);
        return;
    }
    if (strcmp(line, "usbdev") == 0) {
        char debug[300];
        format_usb_device_status(debug, sizeof debug);
        send_rs485_reply(debug);
        return;
    }
    /* ---- usb [0]: body counters (fifo drop, hold, fill) ----------------- */

    if (strncmp(line, "usb", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        if (line[3] == ' ') {
            unsigned z = 1u;
            if (sscanf(line + 4, "%u", &z) != 1 || z != 0u) {
                send_rs485_reply("err:syntax\r\n");
                return;
            }
            clear_audio_stream_statistics();
            clear_usb_statistics();
            rs485_vq_count = 0u;
        }
        {
            char min_s[12];
            uint32_t minf = get_min_stream_buffered_samples();
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
                (unsigned long)get_audio_stream_drop_count(),
                (unsigned long)get_voice_underrun_count(), min_s,
                (unsigned long)get_audio_stream_max_buffered_samples(),
                (unsigned long)get_stream_silent_block_count(),
                (unsigned long)get_stream_start_block_count(), (unsigned long)rs485_vq_count,
                (unsigned long)get_usb_received_frame_count(),
                (unsigned long)get_usb_received_body_count(),
                (unsigned long)get_usb_received_byte_count(), (unsigned long)get_usb_error_count(),
                (unsigned long)get_usb_error_reason_count(0u),
                (unsigned long)get_usb_error_reason_count(1u),
                (unsigned long)get_usb_error_reason_count(2u),
                (unsigned long)get_usb_error_reason_count(3u),
                (unsigned long)get_usb_error_reason_count(4u),
                (unsigned long)get_audio_refill_overlap_count(),
                (unsigned long)get_stream_stale_block_count(),
                (unsigned long)get_stream_future_block_count(),
                (unsigned long)get_stream_full_buffer_count(),
                (unsigned long)get_stream_superseded_block_count());
            send_rs485_reply(usb_b);
        }
        return;
    }

    /* ---- g <ch> <db>: cs4304 dac atten ---------------------------------- */

    if (sscanf(line, "g %u %u", &ch, &val) == 2) {
        (void)ch;
        (void)val;
        process_gain_command(line);
        return;
    }

    /* ---- fk / fk0..fk7: filter pitch-track (before bare f) -------------- */

    if (line[0] == 'f' && line[1] == 'k') {
        process_pitch_tracking_command(line, b, sizeof b);
        return;
    }

    /* ---- f0..f7 / f: lpf on first 8 voices; optional q = df4 g ---------- */

    if (line[0] == 'f') {
        process_filter_command(line, b, sizeof b);
        return;
    }

    if (strcmp(line, "clear") == 0) {
        process_all_voices_command(line);
        return;
    }

    if (line[0] == 'n') {
        console_note_event = 1u;
        if (line[1] == '\0' || line[1] == ' ')
            process_all_voices_command(line);
        else
            process_voice_note_command(line);
        console_note_event = 0u;
        return;
    }

    process_voice_note_command(line);
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
static uint8_t is_command_for_channel_card(char *line)
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

/* ---- receive rs485 command lines ---------------------------------------- */

/** Poll RX, echo typing, run a command on Enter. Non-blocking.
 * Messages addressed to other cards are ignored. */
static bool rs485_line_ready;

void process_rs485_commands(void)
{
    uint8_t c;

    for (;;) {
        if (rs485_line_ready) {
            // Reuse the completed line and RX buffer while this voice's handoff
            // is occupied. Other voices can be posted in the same audio interval.
            if (rs485_line[0] == 'n' &&
                has_prepared_voice_command(parse_voice_index(rs485_line[1]))) return;
            rs485_line_ready = false;
            execute_channel_command((char *)rs485_line);
            rs485_line_len = 0;
        }
        if (!read_rs485_byte(&c)) return;
        if (c == '\r' || c == '\n') {
#if RS485_ECHO
            send_rs485_text("\r\n");
#endif
            rs485_line[rs485_line_len] = '\0';

            /* Execute only commands addressed to this card or broadcast. */
            if (is_command_for_channel_card((char *)rs485_line)) {
                rs485_line_ready = true;
            } else {
                rs485_line_len = 0; // addressed to another card
            }
        } else if (c == 0x08 || c == 0x7F) /* backspace */
        {
            if (rs485_line_len > 0) {
                rs485_line_len--;
#if RS485_ECHO
                send_rs485_text("\b \b");
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
            send_rs485_text(e); /* echo typing */
#endif
        }
    }
}

/* ---- initialize and poll the console ------------------------------------ */

void init_channel_card(void)
{
    init_voice_filters();
    init_voice_envelopes();
    init_sample_storage();
    init_voice_streams();
    init_voices();

    send_rs485_reply("ok: ready — h for cmds\r\n");
}
