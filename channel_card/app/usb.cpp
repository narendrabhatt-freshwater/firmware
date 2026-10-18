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
#include "usb.h"
#include "audio.h"
#include "platform.h"
#include "voice.h"
#include "samples.h"
#include "stream.h"
#include <stdio.h>
#include <string.h>

/* Upload validation errors become the payload of the pending binary reply. */
static char upload_reply[96];
static void set_upload_result(const char *message)
{
    if (message) (void)snprintf(upload_reply, sizeof upload_reply, "%s", message);
}

/* ---- receive attack and wavetable uploads ------------------------------- */

/* Binary upload writes directly into the existing attack bank, then commits length. */

static uint16_t s_id;
static uint32_t s_need;
static uint32_t s_got;
static int8_t *s_dst;

/* ---- query and abort an attack upload ----------------------------------- */

uint8_t is_sample_upload_active(void)
{
    return s_dst != NULL;
}

void abort_sample_upload(void)
{
    set_sample_storage_write_lock(0u);
    s_dst = NULL;
    s_need = 0u;
    s_got = 0u;
}

/* ---- begin sample and wavetable uploads --------------------------------- */

static int begin_sample_upload_to_bank(uint16_t wave_id, uint32_t nbytes)
{
    bool wavetable_upload = wave_id >= ATTACK_BANK_WAVETABLE_FIRST;
    if (wave_id >= ATTACK_BANK_COUNT || nbytes == 0u || nbytes > ATTACK_BANK_BYTES) {
        return -1;
    }
    if (s_dst != NULL || (wavetable_upload != 0u && has_voice_sample_references() != 0u)) {
        return -1;
    }

    set_sample_storage_write_lock(wavetable_upload);
    s_dst = get_sample_write_buffer(wave_id);
    if (s_dst == NULL) {
        set_sample_storage_write_lock(0u);
        return -1;
    }

    s_id = wave_id;
    s_need = nbytes;
    s_got = 0u;
    return 0;
}

int begin_sample_upload(uint16_t wave_id, uint32_t nbytes)
{
    if (wave_id >= ATTACK_BANK_SAMPLE_COUNT) {
        return -1;
    }
    return begin_sample_upload_to_bank(wave_id, nbytes);
}

int begin_wavetable_upload(uint8_t wave, uint32_t nbytes)
{
    if (wave >= ATTACK_BANK_WAVETABLE_COUNT || nbytes < 2u) {
        return -1;
    }
    return begin_sample_upload_to_bank((uint16_t)(ATTACK_BANK_WAVETABLE_FIRST + wave), nbytes);
}

/* ---- receive attack upload data ----------------------------------------- */

uint32_t append_sample_upload_bytes(const uint8_t *buf, uint32_t len)
{
    uint32_t take;
    char msg[48];

    if (buf == NULL || s_dst == NULL) {
        return 0u;
    }

    take = len;
    if (take > (s_need - s_got)) {
        take = s_need - s_got;
    }

    memcpy(s_dst + s_got, buf, take);
    s_got += take;

    if (s_got >= s_need) {
        /* Keep the old length during transfer. Only the completed upload updates
         * metadata; the payload itself overwrites live storage as bytes arrive. */
        memset(s_dst + s_need, 0, ATTACK_BANK_BYTES - s_need);

        if (commit_sample_data(s_id, s_need) != 0) {
            set_upload_result("err:range\r\n");
        } else {
            if (s_id >= ATTACK_BANK_WAVETABLE_FIRST) {
                (void)snprintf(msg, sizeof msg, "ok:wavetable %u\r\n", (unsigned)(s_id - ATTACK_BANK_WAVETABLE_FIRST));
            } else {
                (void)snprintf(msg, sizeof msg, "ok:attack %u\r\n", (unsigned)s_id);
            }
            set_upload_result(msg);
        }
        abort_sample_upload();
    }

    return take;
}

/* ---- dispatch binary usb messages --------------------------------------- */

/* Binary-only CDC: BODY + chunked uploads, with controls/status on RS485. */
static USB_Parser parser;
static uint32_t link_epoch, last_rx_ms, upload_ms;
static uint32_t rx_messages, rx_bytes, blocks, bad;
static uint8_t hello, failed, upload_kind, upload_target;
static uint32_t upload_total, upload_offset;
static uint32_t probe_bytes, probe_blocks, probe_hash;
static uint8_t reply[USB_STREAM_HEADER_BYTES + 128];
static uint16_t reply_size;

/* ---- manage binary uploads and replies ---------------------------------- */

static void abort_usb_upload(void)
{
    if (upload_kind == USB_UPLOAD_SCRIPT)
        abort_voice_script_upload(upload_target);
    abort_sample_upload();
    upload_kind = 0;
    upload_offset = upload_total = 0;
}

/* USB has already checked the target, offset and length. Its existing upload
 * counters own this transaction; the voice runtime only owns bytecode storage. */
static uint32_t append_script_upload_bytes(const uint8_t *data, uint32_t size)
{
    if (append_voice_script_upload_bytes(upload_target, data, size) != 0) {
        set_upload_result("err:vm-container\r\n");
        abort_voice_script_upload(upload_target);
        return size;
    }
    if (upload_offset + size == upload_total) {
        char result[64];
        if (commit_voice_script_upload(upload_target) != 0) {
            (void)snprintf(result, sizeof result, "err:vm %u %u\r\n", (unsigned)upload_target,
                           (unsigned)get_voice_script_fault(upload_target));
        } else {
            (void)snprintf(result, sizeof result, "ok:vm %u %lu %u\r\n", (unsigned)upload_target,
                           (unsigned long)FW_VM_TARGET_CHANNEL,
                           (unsigned)FW_VM_TARGET_CHANNEL_VERSION);
        }
        set_upload_result(result);
    }
    return size;
}

static void prepare_usb_reply(const uint8_t *header, const uint8_t *payload, uint16_t size)
{
    memcpy(reply, header, USB_STREAM_HEADER_BYTES);
    reply[0] = USB_MSG_REPLY;
    /* Request type occupies the reply's session byte; no request IDs are needed
     * because exactly one request/reply exchange may be outstanding. */
    reply[2] = header[0];
    write_usb_uint16_le(reply + 3, size);
    memcpy(reply + USB_STREAM_HEADER_BYTES, payload, size);
    reply_size = (uint16_t)(USB_STREAM_HEADER_BYTES + size);
}

static void reply_usb_result(const uint8_t *h, uint8_t error, const char *message)
{
    uint8_t data[96];
    data[0] = error;
    size_t n = message ? strlen(message) : 0;
    if (n > sizeof(data) - 1) n = sizeof(data) - 1;
    if (n) memcpy(data + 1, message, n);
    prepare_usb_reply(h, data, (uint16_t)(n + 1));
}

static void fail_usb_protocol(const uint8_t *h, const char *message)
{
    ++bad;
    failed = 1;
    hello = 0;
    abort_usb_upload();
    if (h) reply_usb_result(h, 1, message);
    fault_usb_connection();
}

/* ---- dispatch a binary usb message -------------------------------------- */

static uint8_t dispatch_usb_message(const uint8_t *h)
{
    uint16_t n = read_usb_uint16_le(h + 3);
    const uint8_t *p = h + USB_STREAM_HEADER_BYTES;
    if (h[0] == USB_MSG_HELLO) {
        if (hello || n != 1 || p[0] != USB_STREAM_VERSION || h[1] || h[2]) {
            fail_usb_protocol(h, "incompatible HELLO");
            return 1;
        }
        abort_usb_upload();
        uint16_t processed = get_last_stream_body_sequence();
        hello = 1;
        probe_bytes = probe_blocks = 0;
        probe_hash = 2166136261u;
        const uint8_t caps[] = {0,
                                USB_STREAM_VERSION,
                                8,
                                1,
                                (uint8_t)AUDIO_SAMPLE_RATE_HZ,
                                (uint8_t)(AUDIO_SAMPLE_RATE_HZ >> 8),
                                (uint8_t)(AUDIO_SAMPLE_RATE_HZ >> 16),
                                (uint8_t)(AUDIO_SAMPLE_RATE_HZ >> 24),
                                USB_STREAM_PAYLOAD_MAX & 255,
                                USB_STREAM_PAYLOAD_MAX >> 8,
                                USB_STREAM_PRIME_SAMPLES & 255,
                                USB_STREAM_PRIME_SAMPLES >> 8,
                                (uint8_t)(processed & 255),
                                (uint8_t)(processed >> 8)};
        prepare_usb_reply(h, caps, sizeof caps);
        return 1;
    }
    if (!hello) {
        fail_usb_protocol(h, "HELLO required");
        return 1;
    }
    /* Diagnostic sink: no voice, upload, or credit state is changed. An empty
     * probe is an ordered barrier returning totals for this connection. */
    if (h[0] == USB_MSG_PROBE) {
        if (h[1] || h[2]) {
            fail_usb_protocol(h, "invalid PROBE");
            return 1;
        }
        if (n) {
            probe_bytes += n;
            ++probe_blocks;
            for (uint16_t i = 0; i < n; ++i)
                probe_hash = (probe_hash ^ p[i]) * 16777619u;
        } else {
            uint8_t stats[13] = {};
            uint32_t values[] = {probe_bytes, probe_blocks, probe_hash};
            for (unsigned i = 0; i < 3; ++i) {
                write_usb_uint16_le(stats + 1 + i * 4, (uint16_t)values[i]);
                write_usb_uint16_le(stats + 3 + i * 4, (uint16_t)(values[i] >> 16));
            }
            prepare_usb_reply(h, stats, sizeof stats);
        }
        return 1;
    }
    if (h[0] == USB_MSG_BODY) {
        if (!n || h[1] >= 8 || h[2] >= USB_STREAM_SESSION_MOD) {
            fail_usb_protocol(h, "invalid BODY");
            return 1;
        }
        /* Prediction may temporarily run ahead of consumption. Retain this whole
         * block and apply USB backpressure; never overwrite or drop ring samples. */
        if ((get_target_stream_session(h[1]) == h[2] || get_current_stream_session(h[1]) == h[2]) &&
            get_stream_free_samples(h[1]) < ((n + 15u) / 16u) * 16u)
            return 0;
        /* Stale notes are acknowledged/ignored by the ring; no stale data becomes
         * audible. Capacity errors are explicit, never silently dropped. */
        int accepted = write_stream_body_samples(h[1], h[2], (const int8_t *)p, n);
        if (accepted < 0) {
            fail_usb_protocol(h, "BODY ring capacity");
            return 1;
        }
        rx_messages += (uint32_t)(accepted > 0);
        return 1;
    }
    if (h[2]) {
        fail_usb_protocol(h, "invalid upload header");
        return 1;
    }
    upload_reply[0] = 0;
    if (h[0] == USB_MSG_UPLOAD_BEGIN) {
        if (n != 5 || upload_kind) {
            reply_usb_result(h, 1, "upload busy/invalid");
            return 1;
        }
        uint32_t size = read_usb_uint32_le(p + 1);
        int rc = -1;
        if (p[0] == USB_UPLOAD_ATTACK)
            rc = begin_sample_upload(h[1], size);
        else if (p[0] == USB_UPLOAD_WAVE)
            rc = begin_wavetable_upload(h[1], size);
        else if (p[0] == USB_UPLOAD_SCRIPT &&
                 size >= FW_SCRIPT_CONTAINER_HEADER_SIZE &&
                 size <= FW_SCRIPT_CONTAINER_HEADER_SIZE + FW_SCRIPT_MAX_PAYLOAD)
            rc = begin_voice_script_upload(h[1]);
        if (rc == -4) return 0; // Nonblocking VM handoff; retain this complete request.
        if (rc) {
            reply_usb_result(h, 1, "upload rejected (target, size, or active voice)");
            return 1;
        }
        upload_kind = p[0];
        upload_target = h[1];
        upload_total = size;
        upload_offset = 0;
        upload_ms = get_board_time_ms();
        reply_usb_result(h, 0, NULL);
        return 1;
    }
    if (h[0] == USB_MSG_UPLOAD_DATA) {
        if (!upload_kind || h[1] != upload_target || n <= 4 ||
            read_usb_uint32_le(p) != upload_offset ||
            (uint32_t)(n - 4) > upload_total - upload_offset) {
            abort_usb_upload();
            reply_usb_result(h, 1, "upload offset/length");
            return 1;
        }
        uint32_t take = upload_kind == USB_UPLOAD_SCRIPT ? append_script_upload_bytes(p + 4, n - 4)
                                                         : append_sample_upload_bytes(p + 4, n - 4);
        if (take != (uint32_t)(n - 4) || strncmp(upload_reply, "err:", 4) == 0) {
            reply_usb_result(h, 1, upload_reply[0] ? upload_reply : "upload failed");
            abort_usb_upload();
            return 1;
        }
        upload_offset += take;
        upload_ms = get_board_time_ms();
        reply_usb_result(h, 0, NULL);
        if (upload_offset == upload_total) abort_usb_upload();
        return 1;
    }
    if (h[0] == USB_MSG_UPLOAD_ABORT && n == 0) {
        abort_usb_upload();
        reply_usb_result(h, 0, NULL);
        return 1;
    }
    fail_usb_protocol(h, "unknown request");
    return 1;
}

/* ---- initialize the binary usb transport -------------------------------- */

void init_usb_protocol(void)
{
    reset_usb_parser(&parser);
    init_usb_device();
    link_epoch = get_usb_connection_generation();
}

/* ---- receive and process usb messages ----------------------------------- */

void process_usb_messages(void)
{
    uint32_t epoch = get_usb_connection_generation();
    if (epoch != link_epoch) {
        if (!is_usb_connected()) request_stop_all_voices();
        abort_usb_upload();
        reset_usb_parser(&parser);
        hello = failed = 0;
        reply_size = 0;
        link_epoch = epoch;
    }
    if (reply_size) {
        if (!write_usb_bytes(reply, reply_size)) return;
        reply_size = 0;
    }
    if (!is_usb_connected() || failed) return;
    if (parser.used == parser.need) {
        if (!dispatch_usb_message(parser.bytes)) return;
        reset_usb_parser(&parser);
        if (failed || reply_size) return;
    }
    /* Bounded passes keep the main-loop RS485 service responsive. */
    uint32_t budget = 4096;
    while (budget && !reply_size && !failed) {
        /* Never consume beyond this frame: a pending reply must not discard the
         * beginning of the next block from the same USB packet. */
        uint32_t want = parser.need - parser.used;
        if (want > budget) want = budget;
        uint32_t n = read_usb_bytes(parser.bytes + parser.used, want);
        if (get_usb_connection_generation() != link_epoch) return;
        if (!n) break;
        rx_bytes += n;
        budget -= n;
        last_rx_ms = get_board_time_ms();
        int rc = commit_usb_parser_bytes(&parser, (uint16_t)n);
        if (rc < 0) {
            fail_usb_protocol(NULL, "bad header");
            return;
        }
        if (rc > 0) {
            ++blocks;
            if (dispatch_usb_message(parser.bytes)) reset_usb_parser(&parser);
        }
    }
    const TransportTimeouts *timeouts = get_transport_timeouts();
    if (timeouts->usb_partial_frame_ms && parser.used && parser.used < parser.need &&
        (uint32_t)(get_board_time_ms() - last_rx_ms) > timeouts->usb_partial_frame_ms)
        fail_usb_protocol(NULL, "partial frame timeout");
    if (timeouts->usb_upload_idle_ms && upload_kind &&
        (uint32_t)(get_board_time_ms() - upload_ms) > timeouts->usb_upload_idle_ms)
        abort_usb_upload();
}

/* ---- query and clear transport statistics ------------------------------- */

uint32_t get_usb_received_body_count(void)
{
    return rx_messages;
}

uint32_t get_usb_received_byte_count(void)
{
    return rx_bytes;
}

uint32_t get_usb_received_frame_count(void)
{
    return blocks;
}

uint32_t get_usb_error_count(void)
{
    return bad;
}

uint32_t get_usb_error_reason_count(uint8_t reason)
{
    return reason == 4 ? bad : 0;
}

void clear_usb_statistics(void)
{
    rx_messages = rx_bytes = blocks = bad = 0;
}
