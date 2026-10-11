/* Scripted tests for the simulated Channel Card.
 * Each test runs in its own process on a freshly booted card and plays the
 * host over the same two byte streams the real host uses. Time is virtual:
 * it advances only when a test asks for audio, so every run is identical. */

#include "sim.h"
#include "audio.h"
#include "board.h"
#include "channel.h"
#include "samples.h"
#include "stream.h"
#include "usb.h"
#include "voice.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

/* ---- report test results ------------------------------------------------ */

static int s_failures;
static const char *s_program_path;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            ++s_failures;                                                                      \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                                             \
    do {                                                                                       \
        long long a_ = (long long)(actual), e_ = (long long)(expected);                        \
        if (a_ != e_) {                                                                        \
            printf("    FAILED %s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__,       \
                   #actual, a_, e_);                                                           \
            ++s_failures;                                                                      \
        }                                                                                      \
    } while (0)

static bool contains(const std::string &text, const char *needle)
{
    return text.find(needle) != std::string::npos;
}

/* ---- play the rs485 host ------------------------------------------------ */

static std::string rs485_take(void)
{
    std::string out;
    uint8_t chunk[256];
    size_t n;
    while ((n = Sim_Rs485HostRead(chunk, sizeof chunk)) > 0) out.append((const char *)chunk, n);
    return out;
}

/* Send one addressed command line and return whatever the card replied. */
static std::string rs485(const char *command)
{
    std::string line = std::string("c:") + command + "\r";
    Sim_Rs485HostWrite((const uint8_t *)line.data(), line.size());
    Sim_RunMainLoop();
    return rs485_take();
}

typedef struct {
    bool valid;
    uint8_t active, pending;
    uint16_t capacity, body_count;
    uint8_t sequence, body_age_ms;
    uint8_t target[8], current[8];
    uint16_t free_samples[8], refill[8], remaining[8];
} VoiceStatus;

static VoiceStatus vq(void)
{
    VoiceStatus s = {};
    std::string r = rs485("vq");
    const uint8_t *f = (const uint8_t *)r.data();
    if (r.size() != 61u || f[0] != 0xA5u || f[1] != 0x5Au || f[2] != 0x43u || f[3] != 0x0Eu ||
        f[60] != '\n')
        return s;
    s.valid = true;
    s.active = f[4];
    s.pending = f[5];
    s.capacity = (uint16_t)(f[6] | f[7] << 8);
    s.sequence = f[8];
    s.body_age_ms = f[9];
    s.body_count = (uint16_t)(f[10] | f[11] << 8);
    for (int v = 0; v < 8; v++) {
        const uint8_t *p = f + 12 + 6 * v;
        s.target[v] = p[0];
        s.current[v] = p[1];
        s.free_samples[v] = (uint16_t)(p[2] | (p[3] & 15) << 8);
        s.refill[v] = (uint16_t)(p[3] >> 4 | (p[4] & 31) << 4);
        s.remaining[v] = (uint16_t)(p[4] >> 5 | p[5] << 3);
    }
    return s;
}

/* ---- play the usb host -------------------------------------------------- */

typedef struct {
    bool valid;
    uint8_t target, request;
    std::vector<uint8_t> payload;
} UsbReply;

static std::vector<uint8_t> s_usb_in;

static size_t usb_send_raw(const uint8_t *data, size_t size)
{
    size_t sent = Sim_UsbHostWrite(data, size);
    Sim_RunMainLoop();
    return sent;
}

static std::vector<uint8_t> usb_frame(uint8_t type, uint8_t target, uint8_t session,
                                      const uint8_t *payload, size_t size)
{
    std::vector<uint8_t> f = {type, target, session, (uint8_t)size, (uint8_t)(size >> 8)};
    f.insert(f.end(), payload, payload + size);
    return f;
}

/* Returns false if the device would not take the whole frame. */
static bool usb_send(uint8_t type, uint8_t target, uint8_t session, const uint8_t *payload,
                     size_t size)
{
    std::vector<uint8_t> f = usb_frame(type, target, session, payload, size);
    return usb_send_raw(f.data(), f.size()) == f.size();
}

static UsbReply usb_reply(void)
{
    UsbReply r = {};
    uint8_t chunk[256];
    size_t n;
    Sim_RunMainLoop();
    while ((n = Sim_UsbHostRead(chunk, sizeof chunk)) > 0)
        s_usb_in.insert(s_usb_in.end(), chunk, chunk + n);
    if (s_usb_in.size() < 5u) return r;
    size_t size = (size_t)(s_usb_in[3] | s_usb_in[4] << 8);
    if (s_usb_in[0] != USB_MSG_REPLY || s_usb_in.size() < 5u + size) return r;
    r.valid = true;
    r.target = s_usb_in[1];
    r.request = s_usb_in[2];
    r.payload.assign(s_usb_in.begin() + 5, s_usb_in.begin() + 5 + (long)size);
    s_usb_in.erase(s_usb_in.begin(), s_usb_in.begin() + 5 + (long)size);
    return r;
}

static bool reply_ok(const UsbReply &r, uint8_t request)
{
    return r.valid && r.request == request && !r.payload.empty() && r.payload[0] == 0u;
}

static std::string reply_text(const UsbReply &r)
{
    return r.payload.size() > 1u ? std::string(r.payload.begin() + 1, r.payload.end())
                                 : std::string();
}

static UsbReply hello(void)
{
    s_usb_in.clear();
    Sim_UsbAttach();
    const uint8_t version = USB_STREAM_VERSION;
    usb_send(USB_MSG_HELLO, 0, 0, &version, 1);
    return usb_reply();
}

/* Chunked upload; returns the final reply (or the first failing one). */
static UsbReply upload(uint8_t kind, uint8_t target, const std::vector<uint8_t> &data)
{
    uint8_t begin[5] = {kind, (uint8_t)data.size(), (uint8_t)(data.size() >> 8),
                        (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 24)};
    usb_send(USB_MSG_UPLOAD_BEGIN, target, 0, begin, sizeof begin);
    UsbReply r = usb_reply();
    if (!reply_ok(r, USB_MSG_UPLOAD_BEGIN)) return r;
    for (size_t at = 0; at < data.size();) {
        size_t n = data.size() - at > 1020u ? 1020u : data.size() - at;
        std::vector<uint8_t> p = {(uint8_t)at, (uint8_t)(at >> 8), (uint8_t)(at >> 16),
                                  (uint8_t)(at >> 24)};
        p.insert(p.end(), data.begin() + (long)at, data.begin() + (long)(at + n));
        usb_send(USB_MSG_UPLOAD_DATA, target, 0, p.data(), p.size());
        r = usb_reply();
        if (!reply_ok(r, USB_MSG_UPLOAD_DATA)) return r;
        at += n;
    }
    return r;
}

/* ---- run the audio clock ------------------------------------------------ */

static std::vector<int32_t> s_ch1;
static uint32_t s_request = AUDIO_REFILL_FRAMES;

/* Advance the audio clock, delivering frames in s_request-sized requests. */
static void run_frames(uint32_t frames)
{
    std::vector<int32_t> block(s_request * SIM_AUDIO_CHANNELS);
    while (frames) {
        uint32_t n = frames < s_request ? frames : s_request;
        Sim_AudioRequest(block.data(), n);
        for (uint32_t i = 0; i < n; i++) s_ch1.push_back(block[i * SIM_AUDIO_CHANNELS]);
        frames -= n;
    }
    Sim_RunMainLoop();
}

static void run_ms(uint32_t ms)
{
    run_frames(ms * AUDIO_REFILL_FRAMES);
}

static int32_t peak_since(size_t from)
{
    int32_t peak = 0;
    for (size_t i = from; i < s_ch1.size(); i++) {
        int32_t s = s_ch1[i] < 0 ? -s_ch1[i] : s_ch1[i];
        if (s > peak) peak = s;
    }
    return peak;
}

/* ---- prepare a playable card -------------------------------------------- */

static std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return data;
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    return data;
}

static const float ROOT_HZ = 261.625565f;

/* A C4 sine long enough for every test; index 0 is the start of the attack. */
static std::vector<int8_t> make_source(size_t samples)
{
    std::vector<int8_t> source(samples);
    for (size_t i = 0; i < samples; i++)
        source[i] = (int8_t)lrintf(100.0f * sinf(2.0f * (float)M_PI * ROOT_HZ * (float)i /
                                                 (float)AUDIO_SAMPLE_RATE_HZ));
    return source;
}

/* HELLO, load the program into `voices` voices, load sample 0 and its root. */
static bool prepare(const std::vector<int8_t> &source, uint8_t voices)
{
    if (!reply_ok(hello(), USB_MSG_HELLO)) return false;
    std::vector<uint8_t> program = read_file(s_program_path);
    if (program.empty()) {
        printf("    cannot read %s\n", s_program_path);
        return false;
    }
    for (uint8_t v = 0; v < voices; v++)
        if (!reply_ok(upload(USB_UPLOAD_SCRIPT, v, program), USB_MSG_UPLOAD_DATA)) return false;
    std::vector<uint8_t> attack(source.begin(), source.begin() + ATTACK_BANK_LEN);
    if (!reply_ok(upload(USB_UPLOAD_ATTACK, 0, attack), USB_MSG_UPLOAD_DATA)) return false;
    char root[48];
    snprintf(root, sizeof root, "ar 0 %.6f", (double)ROOT_HZ);
    return contains(rs485(root), "ok");
}

/* One streaming voice as the host tracks it. */
typedef struct {
    uint8_t voice, session;
    size_t at;       /* next source index to send */
    bool streaming;
} HostNote;

static HostNote note_on(uint8_t voice, uint8_t key, uint8_t session)
{
    char command[48];
    snprintf(command, sizeof command, "n%u on 0 %u 127 @%u", voice, key, session);
    (void)rs485(command); /* note commands are silent on RS485 */
    HostNote n = {voice, session, SAMPLE_BODY_ORIGIN, true};
    return n;
}

/* Send BODY while the card reports credit, one block at a time, as the host
 * does. Returns the number of blocks sent. */
static uint32_t feed(HostNote *n, const std::vector<int8_t> &source)
{
    uint32_t blocks = 0;
    if (!n->streaming) return 0;
    VoiceStatus s = vq();
    if (!s.valid) return 0;
    uint32_t credit = s.free_samples[n->voice];
    while (n->at < source.size()) {
        uint32_t count = source.size() - n->at > 128u ? 128u : (uint32_t)(source.size() - n->at);
        uint32_t reserve = ((count + 15u) / 16u) * 16u;
        if (credit < reserve) break;
        if (!usb_send(USB_MSG_BODY, n->voice, n->session, (const uint8_t *)source.data() + n->at,
                      count))
            break;
        credit -= reserve;
        n->at += count;
        ++blocks;
    }
    return blocks;
}

/* Run for `ms`, refilling every millisecond of card time. */
static void play_ms(HostNote *n, const std::vector<int8_t> &source, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        feed(n, source);
        run_ms(1);
    }
}

/* ---- tests -------------------------------------------------------------- */

static void test_boot(void)
{
    std::string banner = rs485_take();
    CHECK(contains(banner, "[C] ok: ready"));
    /* Switches are active-low: all off except bypass; hp_ctl is active-high. */
    CHECK_EQ(Sim_Switch(BOARD_SWITCH_BYPASS), 0);
    CHECK_EQ(Sim_Switch(BOARD_SWITCH_HP), 1);
    CHECK_EQ(Sim_Switch(BOARD_SWITCH_VCA), 1);
    CHECK_EQ(Sim_Switch(BOARD_SWITCH_HP_CTL), 0);
    CHECK_EQ(Sim_DacTrim(1), 0);
    CHECK_EQ(Sim_Rs485BusErrors(), 0);
    /* The card renders audio with no host attached. */
    run_ms(20);
    CHECK_EQ(peak_since(0), 0);
    CHECK_EQ(Sim_Millis(), 20);
}

static void test_console(void)
{
    rs485_take();
    CHECK(contains(rs485("vm"), "[C] ok:vm mask 00"));
    CHECK(contains(rs485("VM"), "ok:vm mask 00")); /* input is folded to lower case */
    CHECK(contains(rs485("vm 0"), "ok:vm 0 inactive"));
    CHECK(contains(rs485("vm 9"), "err:syntax"));
    CHECK(contains(rs485("bogus"), "err:unknown"));
    CHECK(contains(rs485("al 0 16"), "err:usb"));
    CHECK(contains(rs485("g 2 10"), "[C] ok"));
    CHECK_EQ(Sim_DacTrim(2), 20);
    CHECK(contains(rs485("g 9 1"), "err:range"));
    CHECK(contains(rs485("reset"), "ok:reset"));
    CHECK(contains(rs485("cpuload 1"), "ok: cpuload 1"));
    CHECK(contains(rs485("cpuload"), "ok: cpuload 1"));
    CHECK(contains(rs485("usbdev"), "ok: usbdev"));

    /* Lines for the Effect Card are ignored; bare and `*:` lines are broadcast. */
    const char other[] = "e:vm\r";
    Sim_Rs485HostWrite((const uint8_t *)other, sizeof other - 1u);
    Sim_RunMainLoop();
    CHECK(rs485_take().empty());
    const char broadcast[] = "*:vm\r";
    Sim_Rs485HostWrite((const uint8_t *)broadcast, sizeof broadcast - 1u);
    Sim_RunMainLoop();
    CHECK(contains(rs485_take(), "ok:vm mask 00"));

    /* Note commands never reply, even when rejected for a missing program. */
    CHECK(rs485("n0 on 60").empty());
    CHECK(rs485("n0 off").empty());
    VoiceStatus s = vq();
    CHECK(s.valid);
    CHECK_EQ(s.active, 0);
    CHECK_EQ(s.capacity, STREAM_RING_SAMPLES);
    CHECK_EQ(Sim_Rs485BusErrors(), 0);
}

static void test_hello(void)
{
    UsbReply r = hello();
    CHECK(reply_ok(r, USB_MSG_HELLO));
    CHECK_EQ(r.payload.size(), 14);
    if (r.payload.size() == 14u) {
        CHECK_EQ(r.payload[1], 2);
        CHECK_EQ(r.payload[2], 8);
        CHECK_EQ(r.payload[3], 1);
        CHECK_EQ(USB_Read32(&r.payload[4]), 48000);
        CHECK_EQ(USB_Read16(&r.payload[8]), 1024);
        CHECK_EQ(USB_Read16(&r.payload[10]), 998);
        CHECK_EQ(USB_Read16(&r.payload[12]), 0);
    }
    /* A second HELLO on the same connection faults the link. */
    const uint8_t version = USB_STREAM_VERSION;
    usb_send(USB_MSG_HELLO, 0, 0, &version, 1);
    r = usb_reply();
    CHECK(r.valid && r.request == USB_MSG_HELLO && r.payload.size() > 1u && r.payload[0] == 1u);
    CHECK(contains(reply_text(r), "incompatible HELLO"));
    CHECK_EQ(Sim_UsbHostSpace(), 0);
    CHECK_EQ(USB_Device_Connected(), 0);
    /* Reopening the port recovers. */
    CHECK(reply_ok(hello(), USB_MSG_HELLO));
    CHECK(Sim_UsbHostSpace() > 0u);
}

static void test_hello_required(void)
{
    Sim_UsbAttach();
    const uint8_t sample = 1;
    usb_send(USB_MSG_BODY, 0, 1, &sample, 1);
    UsbReply r = usb_reply();
    CHECK(r.valid && r.request == USB_MSG_BODY && !r.payload.empty() && r.payload[0] == 1u);
    CHECK(contains(reply_text(r), "HELLO required"));
    CHECK_EQ(USB_Device_Connected(), 0);
}

static void test_bad_header(void)
{
    CHECK(reply_ok(hello(), USB_MSG_HELLO));
    const uint8_t junk[5] = {0x99, 0, 0, 0, 0};
    usb_send_raw(junk, sizeof junk);
    CHECK_EQ(USB_Device_Connected(), 0);
    CHECK_EQ(USB_App_BadCount(), 1);
    CHECK(reply_ok(hello(), USB_MSG_HELLO));
}

static void test_partial_frame_timeout(void)
{
    CHECK(reply_ok(hello(), USB_MSG_HELLO));
    const uint8_t part[3] = {USB_MSG_BODY, 0, 1};
    usb_send_raw(part, sizeof part);
    run_ms(900);
    CHECK_EQ(USB_Device_Connected(), 1);
    run_ms(200);
    CHECK_EQ(USB_Device_Connected(), 0);
}

static void test_upload_script(void)
{
    CHECK(reply_ok(hello(), USB_MSG_HELLO));
    std::vector<uint8_t> program = read_file(s_program_path);
    CHECK(!program.empty());
    if (program.empty()) return;

    /* A corrupted container is rejected and leaves the voice without a program. */
    std::vector<uint8_t> corrupt = program;
    corrupt[corrupt.size() / 2u] ^= 0x55u;
    UsbReply r = upload(USB_UPLOAD_SCRIPT, 0, corrupt);
    CHECK(r.valid && !r.payload.empty() && r.payload[0] == 1u);
    CHECK(contains(rs485("vm 0"), "ok:vm 0 inactive"));
    CHECK_EQ(USB_Device_Connected(), 1);

    CHECK(reply_ok(upload(USB_UPLOAD_SCRIPT, 0, program), USB_MSG_UPLOAD_DATA));
    CHECK(contains(rs485("vm 0"), "ok:vm 0 active"));
    CHECK(contains(rs485("vm"), "ok:vm mask 01"));
    CHECK(reply_ok(upload(USB_UPLOAD_SCRIPT, 5, program), USB_MSG_UPLOAD_DATA));
    CHECK(contains(rs485("vm"), "ok:vm mask 21"));

    /* Wrong offset aborts the upload with an error; the link stays up. */
    uint8_t begin[5] = {USB_UPLOAD_ATTACK, 16, 0, 0, 0};
    usb_send(USB_MSG_UPLOAD_BEGIN, 3, 0, begin, sizeof begin);
    CHECK(reply_ok(usb_reply(), USB_MSG_UPLOAD_BEGIN));
    uint8_t data[8] = {4, 0, 0, 0, 1, 2, 3, 4}; /* offset 4, expected 0 */
    usb_send(USB_MSG_UPLOAD_DATA, 3, 0, data, sizeof data);
    r = usb_reply();
    CHECK(r.valid && !r.payload.empty() && r.payload[0] == 1u);
    CHECK_EQ(USB_Device_Connected(), 1);
    CHECK_EQ(AttackBank_IsLoaded(3), 0);
}

static void test_play_note(void)
{
    std::vector<int8_t> source = make_source(3u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 1));
    HostNote n = note_on(0, 60, 1);
    VoiceStatus s = vq();
    CHECK(s.valid);
    CHECK_EQ(s.target[0], 1);
    CHECK_EQ(s.pending & 1, 1);

    play_ms(&n, source, 500);
    s = vq();
    CHECK_EQ(s.active & 1, 1);
    CHECK_EQ(s.current[0], 1);
    CHECK(s.body_count > 0u);
    CHECK(peak_since(0) > 0x04000000);

    (void)rs485("n0 off");
    size_t release_at = s_ch1.size();
    play_ms(&n, source, 500);
    s = vq();
    CHECK_EQ(s.active & 1, 0);
    CHECK_EQ(peak_since(s_ch1.size() - 480u), 0);
    CHECK(peak_since(release_at) > 0);

    CHECK_EQ(NoteBank_HoldCount(), 0);
    CHECK_EQ(StreamRing_DropCount(), 0);
    CHECK_EQ(StreamRing_FullCount(), 0);
    CHECK_EQ(NoteBank_VmFault(0), 0);
    CHECK_EQ(USB_App_BadCount(), 0);
    CHECK_EQ(USB_Device_Connected(), 1);
    CHECK_EQ(Sim_Rs485BusErrors(), 0);
}

static void test_eight_voices(void)
{
    std::vector<int8_t> source = make_source(2u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 8));
    HostNote notes[8];
    for (uint8_t v = 0; v < 8; v++) notes[v] = note_on(v, (uint8_t)(48 + 3 * v), (uint8_t)(v + 1));
    for (uint32_t ms = 0; ms < 400; ms++) {
        for (uint8_t v = 0; v < 8; v++) feed(&notes[v], source);
        run_ms(1);
    }
    VoiceStatus s = vq();
    CHECK_EQ(s.active, 0xFF);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    CHECK_EQ(StreamRing_DropCount(), 0);
    CHECK(contains(rs485("clear"), "ok:clear rt=1"));
    run_ms(2);
    s = vq();
    CHECK_EQ(s.active, 0);
    CHECK_EQ(s.pending, 0);
    run_ms(20);
    CHECK_EQ(peak_since(s_ch1.size() - 480u), 0);
    /* Programs survive clear. */
    CHECK(contains(rs485("vm"), "ok:vm mask ff"));
}

static void test_steal(void)
{
    std::vector<int8_t> source = make_source(3u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 1));
    HostNote first = note_on(0, 60, 1);
    play_ms(&first, source, 300);
    VoiceStatus s = vq();
    CHECK_EQ(s.current[0], 1);

    /* Replace the playing note: the new session is pending until the script
     * finishes its steal fade, and the old session keeps receiving BODY. */
    HostNote second = note_on(0, 67, 2);
    s = vq();
    CHECK_EQ(s.target[0], 2);
    CHECK_EQ(s.current[0], 1);
    CHECK_EQ(s.pending & 1, 1);
    for (uint32_t ms = 0; ms < 300; ms++) {
        s = vq();
        if (s.current[0] == 1) feed(&first, source);
        feed(&second, source);
        run_ms(1);
    }
    s = vq();
    CHECK_EQ(s.current[0], 2);
    CHECK_EQ(s.pending & 1, 0);
    CHECK_EQ(s.active & 1, 1);
    CHECK(peak_since(s_ch1.size() - 4800u) > 0x04000000);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    CHECK_EQ(StreamRing_DropCount(), 0);
    CHECK_EQ(USB_Device_Connected(), 1);
}

static void test_backpressure(void)
{
    std::vector<int8_t> source = make_source(2u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 1));
    (void)note_on(0, 60, 1);
    /* Ignore credit: push 32 KiB of BODY at a ring that holds 4080 samples. */
    std::vector<uint8_t> stream;
    size_t at = SAMPLE_BODY_ORIGIN;
    uint32_t blocks = 0;
    while (stream.size() < 32768u) {
        std::vector<uint8_t> f = usb_frame(USB_MSG_BODY, 0, 1, (const uint8_t *)&source[at], 128);
        stream.insert(stream.end(), f.begin(), f.end());
        at += 128u;
        ++blocks;
    }
    /* The first write fills the receive queue and the parser moves what fits
     * into the ring. The second refills the queue, and then nothing moves. */
    size_t sent = usb_send_raw(stream.data(), stream.size());
    sent += usb_send_raw(stream.data() + sent, stream.size() - sent);
    CHECK(sent < stream.size());
    CHECK_EQ(Sim_UsbHostSpace(), 0); /* OUT endpoint is NAKing */
    VoiceStatus s = vq();
    CHECK(s.free_samples[0] < 144u);    /* ring full to the block granularity */
    CHECK(s.body_count < blocks);
    /* Playback drains the ring and the rest follows; nothing is lost. */
    for (uint32_t ms = 0; ms < 1000 && sent < stream.size(); ms++) {
        sent += usb_send_raw(stream.data() + sent, stream.size() - sent);
        run_ms(1);
    }
    CHECK_EQ(sent, stream.size());
    run_ms(200);
    s = vq();
    CHECK_EQ(s.body_count, blocks);
    CHECK_EQ(StreamRing_DropCount(), 0);
    CHECK_EQ(StreamRing_FullCount(), 0);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    CHECK_EQ(USB_Device_Connected(), 1);
}

static void test_starved_host(void)
{
    std::vector<int8_t> source = make_source(2u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 1));
    HostNote n = note_on(0, 60, 1);
    play_ms(&n, source, 200);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    /* The host stops sending. The ring holds 85 ms; the card must count the
     * underrun rather than fault, and keep running. */
    run_ms(60);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    run_ms(200);
    CHECK(NoteBank_HoldCount() > 0u);
    CHECK(contains(rs485("usb"), "ok: usb drop 0 hold"));
    CHECK_EQ(USB_Device_Connected(), 1);
    /* Streaming resumes when the host returns. */
    uint32_t holds = (uint32_t)NoteBank_HoldCount();
    play_ms(&n, source, 100);
    uint32_t settled = (uint32_t)NoteBank_HoldCount();
    play_ms(&n, source, 100);
    CHECK_EQ(NoteBank_HoldCount(), settled);
    (void)holds;
}

static void test_late_audio(void)
{
    std::vector<int8_t> source = make_source(2u * AUDIO_SAMPLE_RATE_HZ);
    CHECK(prepare(source, 1));
    HostNote n = note_on(0, 60, 1);
    play_ms(&n, source, 200);
    /* The audio clock stalls and then asks for 60 ms at once, with no host
     * traffic in between. The ring covers it. */
    feed(&n, source);
    s_request = 60u * AUDIO_REFILL_FRAMES;
    run_ms(60);
    s_request = AUDIO_REFILL_FRAMES;
    CHECK_EQ(NoteBank_HoldCount(), 0);
    play_ms(&n, source, 100);
    CHECK_EQ(NoteBank_HoldCount(), 0);
    CHECK_EQ(vq().active & 1, 1);
}

static void test_rs485_overflow(void)
{
    rs485_take();
    /* More than the 8192-byte receive ring without a main-loop pass. */
    std::vector<uint8_t> flood(9000, (uint8_t)'x');
    Sim_Rs485HostWrite(flood.data(), flood.size());
    CHECK_EQ(Uart5Rx_DroppedCount(), 9000 - 8191);
    Sim_RunMainLoop();
    /* Known behaviour, not asserted as correct: with the 96-byte line buffer
     * full, the next command's `c:` prefix cannot resynchronise. The command
     * is lost and the junk line runs as a broadcast (err:unknown). */
    (void)rs485("rs485");
    /* The line ending cleared the buffer; the card answers again. */
    CHECK(contains(rs485("rs485"), "ok: rs485 rxdrop 809 txfail 0 trunc 0"));
}

/* ---- request-size invariance -------------------------------------------- */

/* One note with a release, hashed. Host actions land on fixed frames so the
 * only variable is how many frames each audio request asks for. */
static uint32_t scenario_hash(uint32_t request)
{
    std::vector<int8_t> source = make_source(3u * AUDIO_SAMPLE_RATE_HZ);
    if (!prepare(source, 1)) return 0;
    HostNote n = note_on(0, 60, 1);
    s_request = request;
    for (uint32_t ms = 0; ms < 1000; ms++) {
        if (ms == 600) (void)rs485("n0 off");
        feed(&n, source);
        run_ms(1);
    }
    uint32_t hash = 2166136261u;
    for (int32_t s : s_ch1)
        for (int b = 0; b < 4; b++) hash = (hash ^ (uint8_t)((uint32_t)s >> (8 * b))) * 16777619u;
    if (NoteBank_HoldCount() != 0u || peak_since(0) == 0) return 0;
    return hash;
}

/* ---- run tests in fresh processes --------------------------------------- */

typedef struct {
    const char *name;
    void (*run)(void);
} Test;

static void test_request_size(void);

static const Test tests[] = {
    {"boot", test_boot},
    {"console", test_console},
    {"hello", test_hello},
    {"hello_required", test_hello_required},
    {"bad_header", test_bad_header},
    {"partial_frame_timeout", test_partial_frame_timeout},
    {"upload_script", test_upload_script},
    {"play_note", test_play_note},
    {"eight_voices", test_eight_voices},
    {"steal", test_steal},
    {"backpressure", test_backpressure},
    {"starved_host", test_starved_host},
    {"late_audio", test_late_audio},
    {"rs485_overflow", test_rs485_overflow},
    {"request_size", test_request_size},
};

/* Run fn on a freshly booted card in a child process and return its result. */
static uint32_t isolated(uint32_t (*fn)(uint32_t), uint32_t argument)
{
    int fds[2];
    uint32_t result = 0;
    if (pipe(fds) != 0) return 0;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Sim_Boot();
        result = fn(argument);
        (void)!write(fds[1], &result, sizeof result);
        _exit(0);
    }
    close(fds[1]);
    if (read(fds[0], &result, sizeof result) != (ssize_t)sizeof result) result = 0;
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return result;
}

static void test_request_size(void)
{
    uint32_t reference = isolated(scenario_hash, AUDIO_REFILL_FRAMES);
    CHECK(reference != 0u);
    const uint32_t sizes[] = {1, 7, 64, 480, 4096};
    for (uint32_t size : sizes) {
        uint32_t hash = isolated(scenario_hash, size);
        if (hash != reference) {
            printf("    FAILED: %u frames per request gave hash %08x, expected %08x\n",
                   (unsigned)size, (unsigned)hash, (unsigned)reference);
            ++s_failures;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.bec [test-name]\n", argv[0]);
        return 2;
    }
    s_program_path = argv[1];
    int failed = 0, ran = 0;
    for (const Test &t : tests) {
        if (argc > 2 && strcmp(argv[2], t.name) != 0) continue;
        ++ran;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (t.run != test_request_size) Sim_Boot();
            t.run();
            fflush(stdout);
            _exit(s_failures ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!WIFEXITED(status)) printf("    crashed (signal %d)\n", WTERMSIG(status));
        printf("%-4s %s\n", ok ? "ok" : "FAIL", t.name);
        failed += !ok;
    }
    printf("%d of %d tests passed\n", ran - failed, ran);
    return failed || ran == 0 ? 1 : 0;
}
