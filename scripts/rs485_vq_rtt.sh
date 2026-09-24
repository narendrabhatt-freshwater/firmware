#!/bin/sh
# Linux RS485 vq RTT tester. Stop MAS before running; defaults match MAS.
# Uses the adjacent ARM64 executable when supplied, or a local C compiler.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "${1:-}" != "--build" ] && [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] && [ -x "$script_dir/rs485_vq_rtt.linux-arm64" ]; then
    exec "$script_dir/rs485_vq_rtt.linux-arm64" "$@"
fi
command -v "${CC:-cc}" >/dev/null 2>&1 || {
    echo 'No C compiler found. Put the supplied rs485_vq_rtt.linux-arm64 beside this script on the ARM64 mainframe.' >&2
    exit 1
}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/vq-rtt.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$test_dir/test.c" <<'C_SOURCE'
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_test(int sig) { (void)sig; stopped = 1; }
static int64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int wait_fd(int fd, short events, int64_t deadline) {
    while (!stopped) {
        int64_t left = deadline - now_ns();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, (int)((left + 999999) / 1000000));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) { errno = ETIMEDOUT; return -1; }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
        if (p.revents & events) return 0;
    }
    errno = EINTR;
    return -1;
}
static int valid_frame(const unsigned char *p) {
    if (memcmp(p, "\xa5\x5a\x43\x0c", 4) || p[60] != '\n') return 0;
    unsigned capacity = p[6] | ((unsigned)p[7] << 8);
    if (!capacity || capacity > 8191) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned at = 12 + 6 * i;
        unsigned free_samples = p[at+1] | ((unsigned)(p[at+2] & 31) << 8);
        unsigned refill = (p[at+2] >> 5) | ((unsigned)p[at+3] << 3) | ((unsigned)(p[at+4] & 1) << 11);
        if (free_samples > capacity || refill > 3840 ||
            (((p[4] | p[5]) & (1u << i)) && p[at] == 255)) return 0;
    }
    return 1;
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static unsigned number(const char *s, unsigned min, unsigned max) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || *s == '-' || n < min || n > max) {
        fprintf(stderr, "Invalid numeric argument: %s (range %u..%u)\n", s, min, max);
        exit(2);
    }
    return (unsigned)n;
}
static speed_t baud_speed(unsigned baud) {
    switch (baud) {
        case 9600: return B9600;
        case 115200: return B115200;
#ifdef B921600
        case 921600: return B921600;
#endif
#ifdef B2000000
        case 2000000: return B2000000;
#endif
#ifdef B3000000
        case 3000000: return B3000000;
#endif
        default: fprintf(stderr, "Unsupported baud %u on this OS; use Linux for 3 Mbps.\n", baud); exit(2);
    }
}
int main(int argc, char **argv) {
    const char *port = "/dev/ttyUSB0", *csv_path = NULL;
    unsigned count = 1000, warmup = 10, baud = 3000000, timeout_ms = 100, gap_ms = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("Usage: sh rs485_vq_rtt.sh [--port /dev/ttyUSB0] [--baud 3000000]\n"
                 "  [--count 1000] [--warmup 10] [--timeout-ms 100] [--gap-ms 1] [--csv FILE]\n"
                 "Stop MAS first. Only c:vq + CR is sent. Reads complete 61-byte binary replies.\n"
                 "Reports host-observed RTT, including USB and card delays; stops on first error.\n"
                 "CSV file must not already exist. No Python needed.");
            return 0;
        }
        if (i + 1 >= argc) { fprintf(stderr, "Missing value for %s\n", argv[i]); return 2; }
        const char *opt = argv[i], *value = argv[++i];
        if (!strcmp(opt, "--port")) port = value;
        else if (!strcmp(opt, "--csv")) csv_path = value;
        else if (!strcmp(opt, "--count")) count = number(value, 1, 10000000);
        else if (!strcmp(opt, "--warmup")) warmup = number(value, 0, 1000000);
        else if (!strcmp(opt, "--baud")) baud = number(value, 1, 4000000);
        else if (!strcmp(opt, "--timeout-ms")) timeout_ms = number(value, 1, 60000);
        else if (!strcmp(opt, "--gap-ms")) gap_ms = number(value, 0, 60000);
        else { fprintf(stderr, "Unknown option: %s\n", opt); return 2; }
    }
    speed_t speed = baud_speed(baud);
    double *samples = calloc(count, sizeof(*samples));
    if (!samples) { perror("allocating samples"); return 1; }
    FILE *csv = NULL;
    int fd = -1, saved_ok = 0, result = 1, previous = -1;
    unsigned completed = 0;
    double sum = 0, maximum = 0;
    struct termios saved, settings;
    signal(SIGINT, stop_test);
    signal(SIGTERM, stop_test);
    if (csv_path) {
        int csv_fd = open(csv_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (csv_fd < 0) { perror("creating CSV"); goto done; }
        csv = fdopen(csv_fd, "w");
        if (!csv) { perror("opening CSV stream"); close(csv_fd); goto done; }
        fprintf(csv, "sample,rtt_ms,sequence,active_mask,pending_mask\n");
    }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror(port); goto done; }
    if (flock(fd, LOCK_EX | LOCK_NB) || ioctl(fd, TIOCEXCL)) { perror("locking serial port"); goto done; }
    if (tcgetattr(fd, &saved)) { perror("reading serial settings"); goto done; }
    saved_ok = 1;
    settings = saved;
    cfmakeraw(&settings);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    settings.c_cflag &= ~CRTSCTS;
#endif
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, speed) || cfsetospeed(&settings, speed) ||
        tcsetattr(fd, TCSANOW, &settings) || tcgetattr(fd, &settings)) {
        perror("setting serial baud/8N1"); goto done;
    }
    if (cfgetispeed(&settings) != speed || cfgetospeed(&settings) != speed) {
        fputs("Serial driver did not accept the baud setting\n", stderr); goto done;
    }
    printf("Port: %s; baud: %u; 8N1\n", port, baud);
    char resolved[PATH_MAX], timer[PATH_MAX + 64];
    if (realpath(port, resolved)) {
        const char *name = strrchr(resolved, '/');
        snprintf(timer, sizeof(timer), "/sys/bus/usb-serial/devices/%s/latency_timer", name ? name + 1 : resolved);
        FILE *f = fopen(timer, "r");
        unsigned latency;
        if (f) {
            if (fscanf(f, "%u", &latency) == 1) {
                printf("Adapter latency_timer: %u ms (MAS uses 1 ms)\n", latency);
                if (latency != 1) printf("For a comparable test, set %s to 1 before rerunning.\n", timer);
            }
            fclose(f);
        } else puts("Adapter latency_timer: not exposed/readable");
    }
    printf("Wire-time minimum: %.3f ms (5-byte request + 61-byte reply)\n", 660000.0 / baud);
    puts("Stop MAS first. RTT includes USB/scheduling, card processing and turnaround.\nOnly c:vq + CR is sent; first error stops the test.");
    fflush(stdout);
    if (tcflush(fd, TCIFLUSH)) { perror("flushing old input"); goto done; }
    struct pollfd quiet = {fd, POLLIN, 0};
    int q = poll(&quiet, 1, 100);
    if (q != 0) { fputs("Port not quiet or wait interrupted; stop other bus users and retry.\n", stderr); goto done; }
    for (unsigned i = 0; i < warmup + count && !stopped; ++i) {
        quiet.revents = 0;
        if (poll(&quiet, 1, 0) != 0) { fputs("Unexpected input between requests; possible competing bus user.\n", stderr); goto done; }
        const unsigned char request[] = "c:vq\r";
        unsigned char frame[61];
        size_t sent = 0, got = 0;
        int64_t start = now_ns(), deadline = start + (int64_t)timeout_ms * 1000000;
        while (sent < sizeof(request) - 1) {
            if (wait_fd(fd, POLLOUT, deadline)) { perror("request write wait"); goto done; }
            ssize_t n = write(fd, request + sent, sizeof(request) - 1 - sent);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) { perror("request write"); goto done; }
            sent += (size_t)n;
        }
        while (got < sizeof(frame)) {
            if (wait_fd(fd, POLLIN, deadline)) {
                fprintf(stderr, "Exchange %u: received %zu/61 bytes: %s\n", i + 1, got, strerror(errno)); goto done;
            }
            ssize_t n = read(fd, frame + got, sizeof(frame) - got);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) { fputs("Serial read failed/disconnected\n", stderr); goto done; }
            got += (size_t)n;
        }
        double elapsed = (now_ns() - start) / 1e6;
        if (!valid_frame(frame)) {
            fprintf(stderr, "Exchange %u: malformed vq frame: ", i + 1);
            for (size_t j = 0; j < sizeof(frame); ++j) fprintf(stderr, "%02x", frame[j]);
            fputc('\n', stderr); goto done;
        }
        if (previous >= 0 && frame[8] != (previous + 1) % 256) {
            fprintf(stderr, "vq sequence jumped %d -> %u; possible stale reply, competing client or reset\n", previous, frame[8]); goto done;
        }
        previous = frame[8];
        if (i >= warmup) {
            samples[completed++] = elapsed;
            sum += elapsed;
            if (elapsed > maximum) maximum = elapsed;
            if (csv && fprintf(csv, "%u,%.6f,%u,0x%02x,0x%02x\n", completed, elapsed, frame[8], frame[4], frame[5]) < 0) {
                perror("writing CSV"); goto done;
            }
            if (completed % 100 == 0) { printf("%u/%u: last=%.3f ms; max=%.3f ms\n", completed, count, elapsed, maximum); fflush(stdout); }
        }
        struct timespec gap = {gap_ms / 1000, (long)(gap_ms % 1000) * 1000000};
        while (gap_ms && !stopped && nanosleep(&gap, &gap) && errno == EINTR) {}
    }
    result = stopped ? 130 : 0;
done:
    if (fd >= 0) {
        if (saved_ok && tcsetattr(fd, TCSANOW, &saved)) { perror("restoring serial settings"); result = 1; }
        close(fd);
    }
    if (csv && fclose(csv)) { perror("closing CSV"); result = 1; }
    if (stopped) result = 130;
    printf("Valid measured replies: %u/%u\n", completed, count);
    if (completed) {
        qsort(samples, completed, sizeof(*samples), compare);
        double median = (samples[(completed - 1) / 2] + samples[completed / 2]) / 2;
        printf("RTT ms: min=%.3f median=%.3f mean=%.3f p95=%.3f p99=%.3f max=%.3f\n",
               samples[0], median, sum / completed, samples[(completed * 95u + 99u) / 100u - 1],
               samples[(completed * 99u + 99u) / 100u - 1], samples[completed - 1]);
    }
    puts(result == 0 ? "PASS: all exchanges returned valid, sequential vq frames. Note-off handling is not tested." :
         result == 130 ? "INTERRUPTED: reporting completed exchanges." :
         "FAIL: stopped on first error to avoid attributing a late response to another request.");
    free(samples);
    return result;
}
C_SOURCE
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror "$test_dir/test.c" -o "$test_dir/vq-rtt"
if [ "${1:-}" = "--build" ]; then
    [ "$#" -eq 2 ] || { echo 'Usage: sh rs485_vq_rtt.sh --build OUTPUT' >&2; exit 2; }
    cp "$test_dir/vq-rtt" "$2"
else
    "$test_dir/vq-rtt" "$@"
fi
