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

#include "serial_buffer.h"
#include "channel.h"
#include "usb.h"
#include "audio.h"
#include "platform.h"
#include "voice.h"
#include "application.h"
#include "samples.h"
#include "stream.h"
#include <stdio.h>
#include <string.h>

/* ---- receive attack and wavetable uploads ------------------------------- */

static uint8_t s_active;
static uint16_t s_id;
static uint32_t s_need;
static uint32_t s_got;
static int8_t *s_dst;
static uint8_t s_wavetable_upload;
static uint8_t s_logical_wave;

/* ---- query and abort an attack upload ----------------------------------- */

uint8_t is_sample_upload_active(void)
{
    return s_active;
}

void abort_sample_upload(void)
{
    set_sample_storage_write_lock(0u);
    s_active = 0u;
    s_dst = NULL;
    s_need = 0u;
    s_got = 0u;
    s_wavetable_upload = 0u;
    s_logical_wave = 0u;
}

/* ---- begin sample and wavetable uploads --------------------------------- */

static int begin_sample_upload_to_bank(uint16_t wave_id, uint32_t nbytes, uint8_t wavetable_upload,
                                       uint8_t logical_wave)
{
    if (wave_id >= ATTACK_BANK_COUNT || nbytes == 0u || nbytes > ATTACK_BANK_BYTES) {
        return -1;
    }
    if (s_active != 0u || (wavetable_upload != 0u && has_voice_sample_references() != 0u)) {
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
    s_wavetable_upload = wavetable_upload;
    s_logical_wave = logical_wave;
    s_active = 1u;
    return 0;
}

int begin_sample_upload(uint16_t wave_id, uint32_t nbytes)
{
    if (wave_id >= ATTACK_BANK_SAMPLE_COUNT) {
        return -1;
    }
    return begin_sample_upload_to_bank(wave_id, nbytes, 0u, 0u);
}

int begin_wavetable_upload(uint8_t wave, uint32_t nbytes)
{
    if (wave >= ATTACK_BANK_WAVETABLE_COUNT || nbytes < 2u) {
        return -1;
    }
    return begin_sample_upload_to_bank((uint16_t)(ATTACK_BANK_WAVETABLE_FIRST + wave), nbytes, 1u,
                                       wave);
}

/* ---- receive attack upload data ----------------------------------------- */

uint32_t append_sample_upload_bytes(const uint8_t *buf, uint32_t len)
{
    uint32_t take;
    uint32_t i;
    char msg[48];

    if (s_active == 0u || buf == NULL || s_dst == NULL) {
        return 0u;
    }

    take = len;
    if (take > (s_need - s_got)) {
        take = s_need - s_got;
    }

    {
        uint8_t *dst_bytes = (uint8_t *)s_dst;
        for (i = 0u; i < take; i++) {
            dst_bytes[s_got + i] = buf[i];
        }
    }
    s_got += take;

    if (s_got >= s_need) {
        /* Keep the old length during transfer. Only the completed upload updates
         * metadata; the payload itself overwrites live storage as bytes arrive. */
        for (i = s_need; i < ATTACK_BANK_BYTES; i++)
            s_dst[i] = 0;

        if (commit_sample_data(s_id, s_need) != 0) {
            write_usb_text("err:range\r\n");
        } else {
            if (s_wavetable_upload != 0u) {
                (void)snprintf(msg, sizeof msg, "ok:wavetable %u\r\n", (unsigned)s_logical_wave);
            } else {
                (void)snprintf(msg, sizeof msg, "ok:attack %u\r\n", (unsigned)s_id);
            }
            write_usb_text(msg);
        }
        abort_sample_upload();
    }

    return take;
}

/* ---- retain USB parser and upload progress ------------------------------ */

static USB_Parser parser = {{}, 0, 1};
static uint32_t link_epoch;
static uint32_t last_rx_ms;
static uint32_t upload_ms;
static uint32_t rx_messages;
static uint32_t rx_bytes;
static uint32_t blocks;
static uint32_t bad;
static uint8_t upload_kind;
static uint8_t upload_target;
static uint8_t upload_rejected;
static uint32_t upload_remaining;
static char upload_reply[96];
static uint8_t reply[USB_REPLY_HEADER_BYTES + sizeof upload_reply];
static uint16_t reply_size;

/* ---- retain validation text for the upload reply ------------------------- */

void write_usb_text(const char *s)
{
    if (s)
    {
        snprintf(upload_reply, sizeof upload_reply, "%s", s);
    }
}

/* ---- release an unfinished upload --------------------------------------- */

static void abort_usb_upload(void)
{
    abort_script_upload();
    abort_sample_upload();
    upload_kind = 0;
    upload_remaining = 0;
    upload_rejected = 0;
}

/* ---- reply once with target, upload command and result ------------------- */

static void reply_usb_upload(uint8_t target, uint8_t error, const char *message)
{
    reply[0] = target;
    reply[1] = USB_MSG_UPLOAD;
    reply[2] = error;
    reply_size = USB_REPLY_HEADER_BYTES;
    if (error)
    {
        size_t size = message ? strcspn(message, "\r\n") : 0;
        if (size > sizeof reply - USB_REPLY_HEADER_BYTES - 1)
        {
            size = sizeof reply - USB_REPLY_HEADER_BYTES - 1;
        }
        if (size)
        {
            memcpy(reply + reply_size, message, size);
            reply_size += (uint16_t)size;
        }
        reply[reply_size++] = '\n';
    }
}

/* ---- stop a malformed or incomplete stream without replying to BODY ------ */

static void fail_usb_protocol(void)
{
    ++bad;
    abort_usb_upload();
    usb_serial_buffer.faulted = true;
}

/* ---- accept a BODY frame or prepare the following upload data ------------ */

static uint8_t dispatch_usb_message(const uint8_t *header)
{
    if (header[0] == USB_MSG_BODY)
    {
        uint16_t count = read_usb_uint16_le(header + 3);
        uint8_t voice = header[1];
        uint8_t session = header[2];
        if (!count || voice >= 8 || session >= USB_STREAM_SESSION_MOD)
        {
            fail_usb_protocol();
            return 1;
        }
        if ((get_target_stream_session(voice) == session || get_current_stream_session(voice) == session) &&
            get_stream_free_samples(voice) < ((count + 15u) / 16u) * 16u)
        {
            return 0;
        }
        int accepted = write_stream_body_samples(voice, session,
            (const int8_t *)(header + USB_STREAM_HEADER_BYTES), count);
        if (accepted < 0)
        {
            fail_usb_protocol();
            return 1;
        }
        rx_messages += (uint32_t)(accepted > 0);
        return 1;
    }
    uint32_t total = read_usb_uint32_le(header + 3);
    if (!total || total > USB_UPLOAD_BYTES_MAX)
    {
        reply_usb_upload(header[2], 1, "invalid upload size");
        fail_usb_protocol();
        return 1;
    }
    upload_kind = header[1];
    upload_target = header[2];
    upload_remaining = total;
    upload_reply[0] = 0;
    int result = -1;
    if (upload_kind == USB_UPLOAD_ATTACK)
    {
        result = begin_sample_upload(upload_target, total);
    }
    else if (upload_kind == USB_UPLOAD_WAVE)
    {
        result = begin_wavetable_upload(upload_target, total);
    }
    else if (upload_kind == USB_UPLOAD_SCRIPT)
    {
        result = begin_script_upload(upload_target, total);
    }
    upload_rejected = result != 0;
    if (upload_rejected)
    {
        write_usb_text("upload rejected (kind, target, size, or active voice)");
    }
    upload_ms = get_board_time_ms();
    return 1;
}

/* ---- append consecutive upload bytes and reply only on completion -------- */

static void receive_upload_bytes(const uint8_t *data, uint32_t count)
{
    if (!upload_rejected)
    {
        uint32_t taken = upload_kind == USB_UPLOAD_SCRIPT
            ? append_script_upload_bytes(data, count)
            : append_sample_upload_bytes(data, count);
        if (taken != count || strncmp(upload_reply, "err:", 4) == 0)
        {
            upload_rejected = 1;
            if (!upload_reply[0])
            {
                write_usb_text("upload failed");
            }
            abort_script_upload();
            abort_sample_upload();
        }
    }
    upload_remaining -= count;
    upload_ms = get_board_time_ms();
    if (!upload_remaining)
    {
        reply_usb_upload(upload_target, upload_rejected, upload_reply);
        abort_usb_upload();
    }
}

/* ---- receive fragmented frames and uploads without blocking audio -------- */

void process_usb_messages(void)
{
    uint32_t epoch = usb_serial_buffer.generation;
    if (epoch != link_epoch)
    {
        abort_usb_upload();
        reset_usb_parser(&parser);
        reply_size = 0;
        link_epoch = epoch;
    }
    while (usb_serial_buffer.connected)
    {
        while (usb_serial_buffer.output_size)
        {
            size_t before = usb_serial_buffer.output_size;
            write_usb({usb_serial_buffer.output, usb_serial_buffer.output + before});
            if (usb_serial_buffer.output_size == before)
            {
                return;
            }
        }
        if (reply_size)
        {
            queue_serial_output(usb_serial_buffer, reply, reply_size);
            reply_size = 0;
            continue;
        }
        if (usb_serial_buffer.faulted)
        {
            return;
        }
        if (upload_remaining)
        {
            uint32_t want = upload_remaining < USB_STREAM_PAYLOAD_MAX
                ? upload_remaining : USB_STREAM_PAYLOAD_MAX;
            uint32_t count = read_serial_input(usb_serial_buffer, parser.bytes, want);
            if (!count)
            {
                break;
            }
            rx_bytes += count;
            receive_upload_bytes(parser.bytes, count);
            continue;
        }
        if (parser.used == parser.need)
        {
            if (!dispatch_usb_message(parser.bytes))
            {
                return;
            }
            reset_usb_parser(&parser);
            continue;
        }
        uint32_t count = read_serial_input(usb_serial_buffer, parser.bytes + parser.used,
            parser.need - parser.used);
        if (!count)
        {
            break;
        }
        rx_bytes += count;
        last_rx_ms = get_board_time_ms();
        int result = commit_usb_parser_bytes(&parser, (uint16_t)count);
        if (result < 0)
        {
            fail_usb_protocol();
            return;
        }
        if (result > 0)
        {
            ++blocks;
        }
    }
    if (parser.used && parser.used < parser.need &&
        (uint32_t)(get_board_time_ms() - last_rx_ms) > 1000)
    {
        fail_usb_protocol();
    }
    if (upload_remaining && (uint32_t)(get_board_time_ms() - upload_ms) > 5000)
    {
        reply_usb_upload(upload_target, 1, "incomplete upload");
        fail_usb_protocol();
    }
}

/* ---- query and clear transport statistics ------------------------------- */

uint16_t get_usb_last_body_sequence(void)
{
    return get_last_stream_body_sequence();
}

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
