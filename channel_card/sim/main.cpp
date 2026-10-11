/* sim: the Channel Card application behind two virtual serial ports and a
 * real-time audio clock.
 *
 * A host connects to two Unix sockets in place of the USB CDC port and the
 * RS485 adapter. One thread waits for the three things the card has as
 * interrupts (USB bytes, RS485 bytes, the audio clock), handles whichever
 * fired, runs the main loop until it has nothing left to do, and goes back to
 * waiting. That wait is the processor's sleep. */

#include "sim.h"
#include "audio.h"
#include "channel.h"
#include "stream.h"
#include "usb.h"
#include "voice.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vector>

/* ---- options ------------------------------------------------------------ */

static const char *opt_usb = "/tmp/channel-usb";
static const char *opt_rs485 = "/tmp/channel-rs485";
static const char *opt_wav;
static const char *opt_raw;
static double opt_seconds;
static uint8_t opt_notes, opt_stats;

static void usage(void)
{
    fprintf(stderr,
            "usage: sim [-flags] [-x=value ...]\n"
            "Flags combine, as in  sim -ns -w=out.wav\n"
            "\n"
            "  -n        log each voice starting, being replaced and ending\n"
            "  -s        log one activity line per second\n"
            "  -w=FILE   record DAC channel 1 as 16-bit mono WAV\n"
            "  -o=FILE   stream channel 1 as raw signed 16-bit PCM; -o=- is stdout\n"
            "  -t=S      exit after S seconds of card time\n"
            "  -c=PATH   socket standing in for the USB CDC port (%s)\n"
            "  -l=PATH   socket standing in for the RS485 line (%s)\n"
            "  -h        this help\n"
            "\n"
            "Errors (link faults, underruns, drops, script faults) are always logged.\n",
            opt_usb, opt_rs485);
}

/* Same syntax as voice_bd/options.h: letters after one dash are separate
 * flags, and a letter followed by `=` takes the rest of the word as its
 * value. Returns 0 to run, 1 after help, 2 on a bad option. */
static int parse_options(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *word = argv[i];
        if (word[0] != '-' || word[1] == '\0') {
            fprintf(stderr, "sim: unexpected argument %s\n", word);
            return 2;
        }
        for (const char *c = word + 1; *c; c++) {
            if (c[1] == '=') {
                const char *value = c + 2;
                switch (*c) {
                case 'w': opt_wav = value; break;
                case 'o': opt_raw = value; break;
                case 'c': opt_usb = value; break;
                case 'l': opt_rs485 = value; break;
                case 't': opt_seconds = strtod(value, NULL); break;
                default:
                    fprintf(stderr, "sim: -%c does not take a value\n", *c);
                    return 2;
                }
                break;
            }
            switch (*c) {
            case 'n': opt_notes = 1u; break;
            case 's': opt_stats = 1u; break;
            case 'h':
                usage();
                return 1;
            default:
                fprintf(stderr, "sim: unknown option -%c (try -h)\n", *c);
                return 2;
            }
        }
    }
    return 0;
}

/* ---- logging ------------------------------------------------------------ */

/* Every line carries card time in seconds. */
static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%9.3f  ", Sim_Millis() / 1000.0);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

/* ---- watch card state --------------------------------------------------- */

/* Notes and errors are read from the card's own state after each batch of
 * work, so they report what the card did, not what the host asked for. */
static void log_card_state(void)
{
    static uint8_t active[NOTE_BANK_VOICES], session[NOTE_BANK_VOICES], fault[NOTE_BANK_VOICES];
    static uint8_t started;
    static uint32_t hold, drop, full, rx_dropped, bad;

    if (!started) {
        started = 1u;
        for (uint8_t v = 0; v < NOTE_BANK_VOICES; v++) session[v] = StreamRing_CurrentSession(v);
    }
    for (uint8_t v = 0; v < NOTE_BANK_VOICES; v++) {
        uint8_t now_active = NoteBank_IsActive(v), now_session = StreamRing_CurrentSession(v);
        if (opt_notes) {
            if (now_active && !active[v])
                say("voice %u start: key %u velocity %u session %u sample %u", v,
                    NoteBank_GetKey(v), NoteBank_GetVelocity(v), now_session, NoteBank_GetWaveId(v));
            else if (now_active && now_session != session[v])
                say("voice %u replaced: key %u velocity %u session %u", v, NoteBank_GetKey(v),
                    NoteBank_GetVelocity(v), now_session);
            else if (!now_active && active[v])
                say("voice %u end", v);
        }
        active[v] = now_active;
        session[v] = now_session;
        /* No program loaded is the power-on state, not a fault. */
        static const char *const faults[] = {
            "none",       "upload",         "bad container",     "wrong target", "bad program",
            "bad handler", "budget",        "non-finite value",  "bad host argument",
            "host call",  "no program",     "exception",         "allocation",   "gc",
            "shared vm"};
        uint8_t now_fault = (uint8_t)NoteBank_VmFault(v);
        if (now_fault != fault[v] && now_fault != FW_VM_FAULT_NONE &&
            now_fault != FW_VM_FAULT_NO_PROGRAM)
            say("error: voice %u script fault %u (%s)", v, now_fault,
                now_fault < sizeof faults / sizeof faults[0] ? faults[now_fault] : "?");
        fault[v] = now_fault;
    }

    uint32_t now_hold = NoteBank_HoldCount(), now_drop = StreamRing_DropCount();
    uint32_t now_full = StreamRing_FullCount(), now_rx = Uart5Rx_DroppedCount();
    uint32_t now_bad = USB_App_BadCount();
    if (now_hold > hold)
        say("error: BODY underrun: hold count %lu (+%lu)", (unsigned long)now_hold,
            (unsigned long)(now_hold - hold));
    if (now_drop > drop) say("error: BODY dropped: count %lu", (unsigned long)now_drop);
    if (now_full > full) say("error: BODY ring full: count %lu", (unsigned long)now_full);
    if (now_rx > rx_dropped)
        say("error: RS485 receive overflow: %lu bytes lost", (unsigned long)now_rx);
    if (now_bad > bad) say("error: USB link faulted by the card: count %lu", (unsigned long)now_bad);
    hold = now_hold, drop = now_drop, full = now_full, rx_dropped = now_rx, bad = now_bad;
}

/* ---- time --------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Frames the audio clock has asked for by `now`: the host's monotonic clock
 * at 48 kHz, one steady refill per millisecond as on the card. */
static uint64_t frames_due(uint64_t start, uint64_t now)
{
    return (now - start) / 1000u * AUDIO_SAMPLE_RATE_HZ / 1000000u;
}

static volatile sig_atomic_t s_stop;

static void on_signal(int)
{
    s_stop = 1;
}

/* ---- virtual ports ------------------------------------------------------ */

typedef struct {
    const char *path;
    int listener, client;
    std::vector<uint8_t> out; /* card-to-host bytes not yet written */
} Port;

static int open_listener(const char *path)
{
    struct sockaddr_un address = {};
    if (strlen(path) >= sizeof address.sun_path) {
        fprintf(stderr, "sim: socket path too long: %s\n", path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 1) != 0) {
        fprintf(stderr, "sim: cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* One host at a time, as with a real port. A second connection waits. */
static bool accept_client(Port *port)
{
    if (port->client >= 0) return false;
    int fd = accept(port->listener, NULL, NULL);
    if (fd < 0) return false;
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    port->client = fd;
    port->out.clear();
    return true;
}

static void drop_client(Port *port)
{
    close(port->client);
    port->client = -1;
    port->out.clear();
}

static bool hung_up(int fd)
{
    struct pollfd p = {fd, 0, 0};
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR)) != 0;
}

/* Write queued bytes; returns false if the host went away. */
static bool flush_out(Port *port)
{
    while (!port->out.empty()) {
#if defined(MSG_NOSIGNAL)
        ssize_t n = send(port->client, port->out.data(), port->out.size(), MSG_NOSIGNAL);
#else
        ssize_t n = send(port->client, port->out.data(), port->out.size(), 0);
#endif
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        port->out.erase(port->out.begin(), port->out.begin() + n);
    }
    return true;
}

/* ---- audio output ------------------------------------------------------- */

static FILE *s_wav, *s_raw;
static uint32_t s_wav_frames;
static uint64_t s_rendered;

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void wav_header(FILE *f, uint32_t frames)
{
    rewind(f);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + frames * 2u);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16u);
    put32(f, 0x00010001u); /* PCM, one channel */
    put32(f, AUDIO_SAMPLE_RATE_HZ);
    put32(f, AUDIO_SAMPLE_RATE_HZ * 2u);
    put32(f, 0x00100002u); /* block align 2, 16 bits */
    fwrite("data", 1, 4, f);
    put32(f, frames * 2u);
}

/* The audio interrupt: the DAC takes one refill; keep channel 1. */
static void render_refill(void)
{
    int32_t block[AUDIO_REFILL_FRAMES * SIM_AUDIO_CHANNELS];
    Sim_AudioRequest(block, AUDIO_REFILL_FRAMES);
    s_rendered += AUDIO_REFILL_FRAMES;
    if (s_wav == NULL && s_raw == NULL) return;
    for (uint32_t i = 0; i < AUDIO_REFILL_FRAMES; i++) {
        int16_t sample = (int16_t)(block[i * SIM_AUDIO_CHANNELS] >> 16);
        uint8_t bytes[2] = {(uint8_t)sample, (uint8_t)((uint16_t)sample >> 8)};
        if (s_wav) fwrite(bytes, 1, 2, s_wav);
        if (s_raw) fwrite(bytes, 1, 2, s_raw);
    }
    if (s_wav) s_wav_frames += AUDIO_REFILL_FRAMES;
    if (s_raw) fflush(s_raw);
}

/* ---- serve the hosts ---------------------------------------------------- */

/* Run the main loop until idle and queue its RS485 replies for the host. */
static void run_main_loop(Port *rs485)
{
    uint8_t buffer[1024];
    size_t n;
    (void)Sim_RunMainLoop();
    while ((n = Sim_Rs485HostRead(buffer, sizeof buffer)) > 0)
        if (rs485->client >= 0) rs485->out.insert(rs485->out.end(), buffer, buffer + n);
    log_card_state();
}

/* One pass: wait for a port or the audio clock, then do the work. Returns
 * the nanoseconds spent awake. */
static uint64_t serve(Port *usb, Port *rs485, uint64_t start, uint64_t *late_ns)
{
    uint8_t buffer[4096];
    size_t n;

    /* Sleep until a port has bytes or the next audio request is due. */
    uint64_t now = now_ns();
    uint64_t have = frames_due(start, now), want = s_rendered + AUDIO_REFILL_FRAMES;
    uint64_t due_ns = have >= want ? now
                                   : now + (want - have) * 1000000000u / AUDIO_SAMPLE_RATE_HZ;
    struct timespec timeout = {0, 0};
    if (due_ns > now) {
        timeout.tv_sec = (time_t)((due_ns - now) / 1000000000u);
        timeout.tv_nsec = (long)((due_ns - now) % 1000000000u);
    }
    fd_set readable, writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    int top = -1;
    for (Port *p : {usb, rs485}) {
        if (p->client < 0) {
            FD_SET(p->listener, &readable);
            if (p->listener > top) top = p->listener;
            continue;
        }
        /* A full USB receive queue leaves the bytes with the host. */
        if (p != usb || Sim_UsbHostSpace() > 0u) FD_SET(p->client, &readable);
        if (!p->out.empty()) FD_SET(p->client, &writable);
        if (p->client > top) top = p->client;
    }
    int ready = pselect(top + 1, &readable, &writable, NULL, &timeout, NULL);
    const uint64_t woke = now_ns();
    if (ready < 0) FD_ZERO(&readable);

    /* USB receive interrupt. */
    if (usb->client < 0) {
        if (FD_ISSET(usb->listener, &readable) && accept_client(usb)) {
            Sim_UsbAttach();
            say("usb host opened the port");
        }
    } else {
        bool gone = false;
        if (FD_ISSET(usb->client, &readable)) {
            size_t space = Sim_UsbHostSpace();
            ssize_t got = recv(usb->client, buffer, space < sizeof buffer ? space : sizeof buffer, 0);
            if (got > 0)
                (void)Sim_UsbHostWrite(buffer, (size_t)got);
            else
                gone = got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        } else {
            /* Not reading while the queue is full or the link is faulted;
             * still notice the host closing the port. */
            gone = hung_up(usb->client);
        }
        if (gone) {
            drop_client(usb);
            Sim_UsbDetach();
            say("usb host closed the port");
        }
    }

    /* RS485 receive interrupt. */
    if (rs485->client < 0) {
        if (FD_ISSET(rs485->listener, &readable) && accept_client(rs485))
            say("rs485 host opened the port");
    } else if (FD_ISSET(rs485->client, &readable)) {
        ssize_t got = recv(rs485->client, buffer, sizeof buffer, 0);
        if (got > 0) {
            Sim_Rs485HostWrite(buffer, (size_t)got);
        } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            drop_client(rs485);
            say("rs485 host closed the port");
        }
    }

    /* Audio interrupt: every refill that has come due. A late wake-up
     * leaves several due at once; serve the ports again after 100 ms. */
    uint64_t target = frames_due(start, woke);
    uint32_t burst = 0;
    if (woke > due_ns && woke - due_ns > *late_ns) *late_ns = woke - due_ns;
    while (target >= s_rendered + AUDIO_REFILL_FRAMES && burst < AUDIO_SAMPLE_RATE_HZ / 10u) {
        render_refill();
        burst += AUDIO_REFILL_FRAMES;
    }

    /* Main loop, then hand replies to the host. */
    run_main_loop(rs485);
    if (usb->client >= 0 && usb->out.empty()) {
        while ((n = Sim_UsbHostRead(buffer, sizeof buffer)) > 0)
            usb->out.insert(usb->out.end(), buffer, buffer + n);
        run_main_loop(rs485); /* a reply may have been waiting for queue space */
    }
    if (usb->client >= 0 && !flush_out(usb)) {
        drop_client(usb);
        Sim_UsbDetach();
        say("usb host closed the port");
    }
    if (rs485->client >= 0 && !flush_out(rs485)) {
        drop_client(rs485);
        say("rs485 host closed the port");
    }
    return now_ns() - woke;
}

/* ---- run the card ------------------------------------------------------- */

int main(int argc, char **argv)
{
    int parsed = parse_options(argc, argv);
    if (parsed != 0) return parsed == 1 ? 0 : 2;

    Port usb = {opt_usb, open_listener(opt_usb), -1, {}};
    Port rs485 = {opt_rs485, open_listener(opt_rs485), -1, {}};
    if (usb.listener < 0 || rs485.listener < 0) return 1;
    if (opt_wav) {
        s_wav = fopen(opt_wav, "wb");
        if (s_wav == NULL) {
            fprintf(stderr, "sim: cannot write %s\n", opt_wav);
            return 1;
        }
        wav_header(s_wav, 0);
    }
    if (opt_raw) {
        s_raw = strcmp(opt_raw, "-") == 0 ? stdout : fopen(opt_raw, "wb");
        if (s_raw == NULL) {
            fprintf(stderr, "sim: cannot write %s\n", opt_raw);
            return 1;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Sim_Boot();
    fprintf(stderr, "sim: Channel Card on USB %s and RS485 %s\n", opt_usb, opt_rs485);

    const uint64_t start = now_ns();
    uint64_t busy_ns = 0, stats_at = start + 1000000000u, late_ns = 0, wakeups = 0;
    SimCounters last = *Sim_Counters();
    while (!s_stop) {
        busy_ns += serve(&usb, &rs485, start, &late_ns);
        ++wakeups;
        if (now_ns() >= stats_at) {
            const SimCounters *c = Sim_Counters();
            if (opt_stats)
                say("awake %4.1f%%  wakeups %5llu  audio %4llu  usb %4llu  rs485 %4llu  "
                    "main %5llu  late %5.2fms  hold %lu  drop %lu",
                    (double)busy_ns / 1e7, (unsigned long long)wakeups,
                    (unsigned long long)(c->refills - last.refills),
                    (unsigned long long)(c->usb_events - last.usb_events),
                    (unsigned long long)(c->rs485_events - last.rs485_events),
                    (unsigned long long)(c->main_passes - last.main_passes), (double)late_ns / 1e6,
                    (unsigned long)NoteBank_HoldCount(), (unsigned long)StreamRing_DropCount());
            last = *c;
            busy_ns = wakeups = late_ns = 0;
            stats_at += 1000000000u;
        }
        if (opt_seconds > 0.0 && (double)s_rendered >= opt_seconds * AUDIO_SAMPLE_RATE_HZ) break;
    }

    if (s_wav) {
        wav_header(s_wav, s_wav_frames);
        fclose(s_wav);
    }
    if (s_raw && s_raw != stdout) fclose(s_raw);
    unlink(opt_usb);
    unlink(opt_rs485);
    const SimCounters *c = Sim_Counters();
    fprintf(stderr,
            "sim: stopped after %.2f s of card time: %llu audio refills, %llu USB and %llu RS485 "
            "deliveries, hold %lu, drop %lu\n",
            (double)s_rendered / AUDIO_SAMPLE_RATE_HZ, (unsigned long long)c->refills,
            (unsigned long long)c->usb_events, (unsigned long long)c->rs485_events,
            (unsigned long)NoteBank_HoldCount(), (unsigned long)StreamRing_DropCount());
    return 0;
}
