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

#define main simulator_main
#include "../../../simulator/main.cpp"
#undef main
#include <cassert>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <thread>

// Capture the real wrapper callback without opening a sound device.
static PaStreamCallback *callback;
static void *context;
static int started_fd = -1;
static std::thread callback_thread;
PaError Pa_Initialize() { return paNoError; }
PaError Pa_Terminate() { return paNoError; }
PaError Pa_OpenDefaultStream(PaStream **stream, int inputs, int outputs,
    PaSampleFormat format, double rate, unsigned long frames,
    PaStreamCallback *cb, void *user)
{
    assert(inputs == 0 && outputs == 1 && format == paInt32);
    assert(rate == 48000 && frames == BUF_COUNT * AUDIO_BUFFER_FRAMES);
    *stream = reinterpret_cast<PaStream *>(1);
    callback = cb;
    context = user;
    return paNoError;
}
PaError Pa_StartStream(PaStream *)
{
    assert(write_index == 0);
    for (const auto &buffer : audio_buffers)
    {
        bool populated = false;
        for (int32_t sample : buffer) populated |= sample != 0;
        assert(populated);
    }
    if (started_fd >= 0) assert(write(started_fd, "S", 1) == 1);
    return paNoError;
}
PaError Pa_CloseStream(PaStream *) {
    if (callback_thread.joinable()) callback_thread.join();
    return paNoError;
}

static void close_select()
{
    for (int fd : audio_requests) close(fd);
}

static void close_serial_port(int port, int client)
{
    close(port);
    close(client);
}

static void start_test_audio()
{
    setup_select();
    open_audio();
}

static void stop_test_audio()
{
    Pa_CloseStream(stream);
    Pa_Terminate();
    close_select();
}

static int32_t sample_value(unsigned index)
{
    return static_cast<int32_t>(std::sin(index * 6.28318530717958647692 * 440.0 / 48000.0)
                                * (INT32_MAX / 4.0));
}

static void check_generator()
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        setup_select();
        for (unsigned block = 0; block < BUF_COUNT; ++block) generate_audio_samples();
        assert(write_index == 0);
        int32_t saved[BUF_COUNT][AUDIO_BUFFER_FRAMES];
        std::memcpy(saved, audio_buffers, sizeof saved);
        assert(write(audio_requests[1], "x", 1) == 1); // Release one slot.
        wait_select();
        generate_audio_samples();
        assert(write_index == 1);
        for (unsigned i = 0; i < AUDIO_BUFFER_FRAMES; ++i)
            assert(std::abs(audio_buffers[0][i] - sample_value(BUF_COUNT * AUDIO_BUFFER_FRAMES + i)) <= 2);
        assert(std::memcmp(saved[1], audio_buffers[1], sizeof saved - sizeof saved[0]) == 0);
        close_select();
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void main_pass()
{
    wait_select();
    process_serial(rs485_port, rs485_input);
    process_serial(usb_port, usb_input);
    generate_audio_samples();
}

static void check_audio_callback()
{
    static_assert(BUF_COUNT == 10);
    constexpr unsigned count = BUF_COUNT * AUDIO_BUFFER_FRAMES;
    int usb_client = -1;
    int rs485_client = -1;
    open_serial_port(usb_port, usb_client, "USB");
    open_serial_port(rs485_port, rs485_client, "RS485");
    start_test_audio();
    assert(fcntl(audio_requests[1], F_GETFL) & O_NONBLOCK);
    assert(write_index == 0);
    unsigned consumed = 0;
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        std::vector<int32_t> output(count + 2, 456);
        // No producer is running: the callback must return the prefilled batch.
        assert(callback(nullptr, output.data() + 1, count, nullptr, 0, context) == paContinue);
        assert(output.front() == 456 && output.back() == 456);
        for (unsigned i = 0; i < count; ++i)
            assert(std::abs(output[1 + i] - sample_value(consumed + i)) <= 2);
        consumed += count;
        int remaining;
        assert(ioctl(audio_requests[0], FIONREAD, &remaining) == 0 && remaining == BUF_COUNT);
        for (unsigned block = 0; block < BUF_COUNT; ++block) {
            if (block == 1) {
                assert(write(usb_client, "U", 1) == 1);
                assert(write(rs485_client, "R", 1) == 1);
            }
            main_pass();
            assert(write_index == (block + 1) % BUF_COUNT);
            assert(ioctl(audio_requests[0], FIONREAD, &remaining) == 0);
            assert(remaining == static_cast<int>(BUF_COUNT - block - 1));
            if (block == 1) {
                assert(usb_input.size() == cycle + 1 && usb_input.back() == 'U');
                assert(rs485_input.size() == cycle + 1 && rs485_input.back() == 'R');
            }
        }
        pollfd empty{audio_requests[0], POLLIN, 0};
        assert(poll(&empty, 1, 0) == 0);
    }
    stop_test_audio();
    consumed += count; // The last refill was generated but never played.
    start_test_audio();
    constexpr unsigned batches = 64;
    std::vector<int32_t> continuous(batches * count);
    int next_callback[2];
    assert(pipe(next_callback) == 0);
    callback_thread = std::thread([&] {
        for (unsigned batch = 0; batch < batches; ++batch) {
            assert(callback(nullptr, continuous.data() + batch * count, count,
                            nullptr, 0, context) == paContinue);
            // Test pacing only: allow the producer to finish before the next callback.
            char token;
            assert(read(next_callback[0], &token, 1) == 1);
        }
    });
    for (unsigned batch = 0; batch < batches; ++batch) {
        for (unsigned block = 0; block < BUF_COUNT; ++block) main_pass();
        assert(write(next_callback[1], "x", 1) == 1);
    }
    callback_thread.join();
    close(next_callback[0]);
    close(next_callback[1]);
    for (unsigned i = 0; i < continuous.size(); ++i)
        assert(std::abs(continuous[i] - sample_value(consumed + i)) <= 2);
    stop_test_audio();
    close_serial_port(usb_port, usb_client);
    close_serial_port(rs485_port, rs485_client);
    usb_input.clear();
    rs485_input.clear();
}

static void check_serial_select()
{
    int usb_client = -1;
    int rs485_client = -1;
    open_serial_port(usb_port, usb_client, "USB");
    open_serial_port(rs485_port, rs485_client, "RS485");
    setup_select();
    for (int client : {usb_client, rs485_client}) {
        auto &input = client == usb_client ? usb_input : rs485_input;
        const uint8_t bytes[] = {0, '\r', '\n', 3, 17, 19, 255};
        assert(write(client, bytes, sizeof bytes) == sizeof bytes);
        while (input.size() < sizeof bytes)
        {
            const auto before = write_index;
            main_pass();
            assert(write_index == (before + 1) % BUF_COUNT);
        }
        assert(std::memcmp(input.data(), bytes, sizeof bytes) == 0);
        // More than a single read preserves byte order and previously read data.
        std::vector<uint8_t> more(777);
        for (size_t i = 0; i < more.size(); ++i) more[i] = i;
        assert(write(client, more.data(), more.size()) == static_cast<ssize_t>(more.size()));
        while (input.size() < sizeof bytes + more.size())
        {
            const auto before = write_index;
            main_pass();
            assert(write_index == (before + 1) % BUF_COUNT);
        }
        assert(input.size() == sizeof bytes + more.size());
        assert(std::memcmp(input.data(), bytes, sizeof bytes) == 0);
        assert(std::memcmp(input.data() + sizeof bytes, more.data(), more.size()) == 0);
    }
    close_select();
    close_serial_port(usb_port, usb_client);
    close_serial_port(rs485_port, rs485_client);
    usb_input.clear();
    rs485_input.clear();
}

static void check_shutdown()
{
    for (int signal : {SIGINT, SIGTERM}) {
        int ready[2];
        assert(pipe(ready) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            close(ready[0]);
            started_fd = ready[1];
            _exit(simulator_main());
        }
        close(ready[1]);
        pollfd event{ready[0], POLLIN, 0};
        assert(poll(&event, 1, 3000) == 1);
        char started;
        assert(read(ready[0], &started, 1) == 1 && started == 'S');
        assert(kill(child, signal) == 0);
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == signal);
        close(ready[0]);
    }
}

static void check_serial_ports()
{
    int usb_port = -1;
    int usb_client_port = -1;
    int rs485_port = -1;
    int rs485_client_port = -1;
    open_serial_port(usb_port, usb_client_port, "USB");
    open_serial_port(rs485_port, rs485_client_port, "RS485");
    assert(usb_port != rs485_port && usb_client_port != rs485_client_port);
    for (int fd : {usb_port, rs485_port}) {
        int &simulator_port = fd == usb_port ? usb_port : rs485_port;
        int &client_port = fd == usb_port ? usb_client_port : rs485_client_port;
        termios mode;
        assert(tcgetattr(client_port, &mode) == 0);
        assert(!(mode.c_lflag & (ICANON | ECHO | ISIG)));
        assert(!(mode.c_iflag & (ICRNL | INLCR | IXON)) && !(mode.c_oflag & OPOST));
        const unsigned char bytes[] = {0, '\r', '\n', 3, 17, 19, 255};
        for (bool to_simulator : {true, false}) {
            int from = to_simulator ? client_port : simulator_port;
            int to = to_simulator ? simulator_port : client_port;
            assert(write(from, bytes, sizeof bytes) == sizeof bytes);
            unsigned char received[sizeof bytes];
            size_t count = 0;
            while (count < sizeof bytes) {
                pollfd readable{to, POLLIN, 0};
                assert(poll(&readable, 1, 1000) == 1);
                ssize_t n = read(to, received + count, sizeof received - count);
                assert(n > 0);
                count += n;
            }
            assert(std::memcmp(bytes, received, sizeof bytes) == 0);
        }
        int closed_simulator_port = simulator_port;
        int closed_client_port = client_port;
        close_serial_port(simulator_port, client_port);
        assert(fcntl(closed_simulator_port, F_GETFD) == -1 && errno == EBADF);
        assert(fcntl(closed_client_port, F_GETFD) == -1 && errno == EBADF);
    }
}

int main()
{
    setup_select();
    assert(write(audio_requests[1], "\0\0", 2) == 2);
    wait_select();
    wait_select(); // Posting before select retains the token.
    close_select();
    check_generator();
    check_audio_callback();
    check_serial_select();
    check_serial_ports();
    check_shutdown();
}
