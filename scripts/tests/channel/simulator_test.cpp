#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <chrono>

static ssize_t test_write(int port, const void *data, size_t size);
#define write test_write
#define main simulator_main
#include "../../../channel/main.cpp"
#undef main
#undef write
#include "usb.h"
#include "samples.h"
#include "voice.h"
#include "stream.h"
#include "transport.h"
#include "serial_buffer.h"

static bool run_main_loop;
static size_t write_limit = 3;
static unsigned blocked_writes;
static unsigned partial_writes;
static unsigned failed_writes;
static int usb_client;
static int rs485_client;
static std::vector<uint8_t> usb_reply;
static std::vector<uint8_t> serial_reply;

/* ---- constrain platform writes while using real raw PTYs ---------------- */

static ssize_t test_write(int port, const void *data, size_t size)
{
    if (port == usb_port || port == rs485_port)
    {
        if (blocked_writes)
        {
            --blocked_writes;
            ++failed_writes;
            errno = EAGAIN;
            return -1;
        }
        if (size > write_limit)
        {
            ++partial_writes;
            size = write_limit;
        }
    }
    return ::write(port, data, size);
}

/* ---- open test audio without a physical device -------------------------- */

PaError Pa_Initialize()
{
    return paNoError;
}

/* ---- provide a default output device for the audio setup ---------------- */

PaDeviceIndex Pa_GetDefaultOutputDevice()
{
    return 0;
}

/* ---- provide the recommended low output latency ------------------------- */

const PaDeviceInfo *Pa_GetDeviceInfo(PaDeviceIndex device)
{
    assert(device == 0);
    static PaDeviceInfo info{};
    info.defaultLowOutputLatency = 0.002;
    return &info;
}

/* ---- verify low latency output with variable callback sizes ------------- */

PaError Pa_OpenStream(PaStream **output, const PaStreamParameters *inputs,
    const PaStreamParameters *outputs, double rate, unsigned long frames,
    PaStreamFlags flags, PaStreamCallback *callback, void *)
{
    assert(inputs == nullptr && outputs->channelCount == 1);
    assert(outputs->device == 0 && outputs->sampleFormat == paInt32);
    assert(outputs->suggestedLatency == Pa_GetDeviceInfo(0)->defaultLowOutputLatency);
    assert(flags == paNoFlag);
    assert(rate == 48000 && frames == paFramesPerBufferUnspecified);
    assert(callback == audio_callback);
    *output = reinterpret_cast<PaStream *>(1);
    return paNoError;
}

/* ---- verify startup initializes and prefills silent audio ---------------- */

PaError Pa_StartStream(PaStream *)
{
    assert(write_index == BUF_COUNT - 1 && read_index == 0);
    for (const auto &buffer : audio_buffers)
    {
        for (int32_t sample : buffer)
        {
            assert(sample == 0);
        }
    }
    if (run_main_loop)
    {
        std::thread([]
        {
            int32_t output[512];
            while (true)
            {
                audio_callback(nullptr, output, 512, nullptr, 0, nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }).detach();
    }
    return paNoError;
}

/* ---- collect all currently available client bytes ------------------------ */

static void collect(int client, std::vector<uint8_t> &bytes)
{
    uint8_t data[4096];
    ssize_t count;
    while ((count = read(client, data, sizeof data)) > 0)
    {
        bytes.insert(bytes.end(), data, data + count);
    }
}

/* ---- receive input and service application work through audio generation - */

static void settle()
{
    unsigned idle = 0;
    for (unsigned pass = 0; pass < 10000; ++pass)
    {
        fd_set ready{};
        FD_SET(audio_requests[0], &ready);
        FD_SET(usb_port, &ready);
        FD_SET(rs485_port, &ready);
        timeval timeout{};
        int highest = std::max(audio_requests[0], std::max(usb_port, rs485_port));
        int events = select(highest + 1, &ready, nullptr, nullptr, &timeout);
        if (FD_ISSET(usb_port, &ready))
        {
            process_serial(usb_port, usb_input);
            received_usb(usb_input);
            usb_input.clear();
        }
        if (FD_ISSET(rs485_port, &ready))
        {
            process_serial(rs485_port, rs485_input);
            received_serial(rs485_input);
            rs485_input.clear();
        }
        if (FD_ISSET(audio_requests[0], &ready))
        {
            char token;
            assert(read(audio_requests[0], &token, 1) == 1 && token == 0);
            generate_samples(audio_buffers[write_index]);
            write_index = (write_index + 1) % BUF_COUNT;
        }
        else
        {
            int32_t buffer[SPB];
            generate_samples(buffer);
        }
        collect(usb_client, usb_reply);
        collect(rs485_client, serial_reply);
        if (!events && !usb_serial_buffer.input_size && !rs485_serial_buffer.input_size &&
            !usb_serial_buffer.output_size && !rs485_serial_buffer.output_size &&
            !has_pending_rs485_reply())
        {
            if (++idle == 2)
            {
                return;
            }
        }
        else
        {
            idle = 0;
        }
    }
    assert(false && "pending work failed to settle");
}

/* ---- send fragmented bytes through a real platform endpoint -------------- */

static void send_bytes(int client, const std::vector<uint8_t> &bytes, size_t fragment)
{
    for (size_t offset = 0; offset < bytes.size();)
    {
        size_t count = std::min(fragment, bytes.size() - offset);
        ssize_t sent = ::write(client, bytes.data() + offset, count);
        assert(sent > 0 || errno == EAGAIN);
        if (sent > 0)
        {
            offset += size_t(sent);
        }
        settle();
    }
}

/* ---- encode the USB message header ------------------------------- */

static std::vector<uint8_t> frame(uint8_t type, const std::vector<uint8_t> &payload,
    uint8_t target = 0, uint8_t session = 0)
{
    if (type == USB_MSG_UPLOAD)
    {
        assert(payload.size() >= 5);
        std::vector<uint8_t> bytes{type, payload[0], target};
        bytes.insert(bytes.end(), payload.begin() + 1, payload.end());
        return bytes;
    }
    std::vector<uint8_t> bytes{type, target, session, uint8_t(payload.size()),
        uint8_t(payload.size() >> 8)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

/* ---- verify a complete successful protocol reply ------------------------ */

static void expect_reply(uint8_t type, size_t size = 3)
{
    assert(usb_reply.size() == size);
    assert(type == USB_MSG_UPLOAD && usb_reply[1] == type && usb_reply[2] == 0);
    usb_reply.clear();
}

/* ---- check independent parsers with fragmented and combined messages ----- */

static void check_serial()
{
    settle();
    assert(!serial_reply.empty() && usb_reply.empty());
    serial_reply.clear();
    unsigned before = write_index;
    blocked_writes = 5;
    send_bytes(usb_client, frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 16, 32}, 7), 1);
    expect_reply(USB_MSG_UPLOAD);
    assert(serial_reply.empty());
    auto first = frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 16, 32}, 7);
    auto second = frame(USB_MSG_UPLOAD, {2, 2, 0, 0, 0, 32, 16}, 7);
    first.insert(first.end(), second.begin(), second.end());
    send_bytes(usb_client, first, first.size());
    assert(usb_reply == std::vector<uint8_t>({7, 2, 0, 7, 2, 0}));
    usb_reply.clear();
    const std::string commands = "c:g 1 12\rc:g 1 13\r";
    for (size_t fragment : {size_t(1), commands.size()})
    {
        blocked_writes = 2;
        send_bytes(rs485_client, {commands.begin(), commands.end()}, fragment);
        assert(std::string(serial_reply.begin(), serial_reply.end()) == "[C] ok\r\n[C] ok\r\n");
        assert(usb_reply.empty());
        serial_reply.clear();
    }
    assert(write_index == before);
    assert(partial_writes && failed_writes == 9);
}

/* ---- report filter fields consistently for normal, tracked and bypassed voices */

static void check_filter_replies()
{
    const std::string commands = "c:fk0 0\rc:f0 1000 0.7\r";
    send_bytes(rs485_client, {commands.begin(), commands.end()}, 1);
    assert(std::string(serial_reply.begin(), serial_reply.end()) ==
        "[C] ok: fk0 0.00\r\n"
        "[C] ok: f0 base 1000.0 effective 1000.0 bypass 0 k 0.00 q 0.70\r\n");
    serial_reply.clear();
    const std::string bypass = "c:f0 0\r";
    send_bytes(rs485_client, {bypass.begin(), bypass.end()}, bypass.size());
    assert(std::string(serial_reply.begin(), serial_reply.end()).find("bypass 1 k 0.00 q 0.70") != std::string::npos);
    serial_reply.clear();
    const std::string tracked = "c:fk0 1\rc:f0 2000\r";
    send_bytes(rs485_client, {tracked.begin(), tracked.end()}, tracked.size());
    const std::string reply(serial_reply.begin(), serial_reply.end());
    assert(reply.find("base 2000.0 effective ") != std::string::npos);
    assert(reply.find("bypass 0 k 1.00 q 0.70") != std::string::npos);
    serial_reply.clear();
    const std::string reset = "c:fk0 0\rc:f0 0\r";
    send_bytes(rs485_client, {reset.begin(), reset.end()}, reset.size());
    serial_reply.clear();
}

/* ---- reply to complete requests without waiting for audio generation ----- */

static void check_pending_commands()
{
    const std::string commands = "c:g 1 12\rc:g 1 13\r";
    received_serial({commands.begin(), commands.end()});
    collect(rs485_client, serial_reply);
    assert(std::string(serial_reply.begin(), serial_reply.end()) == "[C] ok\r\n[C] ok\r\n");
    assert(!rs485_serial_buffer.input_size && !rs485_serial_buffer.output_size);
    assert(!has_pending_rs485_reply());
    serial_reply.clear();

    const std::string partial = "c:g 1 ";
    received_serial({partial.begin(), partial.end()});
    collect(rs485_client, serial_reply);
    assert(serial_reply.empty());
    received_serial({'1', '2', '\r'});
    collect(rs485_client, serial_reply);
    assert(std::string(serial_reply.begin(), serial_reply.end()) == "[C] ok\r\n");
    serial_reply.clear();

    received_usb(frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 16, 32}, 7));
    collect(usb_client, usb_reply);
    expect_reply(USB_MSG_UPLOAD);
}

/* ---- retain messages larger than the parser buffers without losing bytes - */

static void check_input_capacity()
{
    std::vector<uint8_t> input;
    std::string expected;
    const std::string command = "c:g 1 12\r";
    for (unsigned i = 0; i < 1000; ++i)
    {
        input.insert(input.end(), command.begin(), command.end());
        expected += "[C] ok\r\n";
    }
    assert(input.size() > sizeof rs485_serial_buffer.input);
    received_serial(input);
    settle();
    assert(std::string(serial_reply.begin(), serial_reply.end()) == expected);
    serial_reply.clear();
    input.clear();
    for (unsigned i = 0; i < 1500; ++i)
    {
        auto bytes = frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, uint8_t(i), uint8_t(i + 1)}, 7);
        input.insert(input.end(), bytes.begin(), bytes.end());
    }
    auto query = frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 16, 32}, 7);
    input.insert(input.end(), query.begin(), query.end());
    assert(input.size() > sizeof usb_serial_buffer.input);
    received_usb(input);
    settle();
    assert(usb_reply.size() == 1501 * 3);
    for (size_t offset = 0; offset < usb_reply.size(); offset += 3)
    {
        assert(usb_reply[offset] == 7 && usb_reply[offset + 1] == 2 && usb_reply[offset + 2] == 0);
    }
    usb_reply.clear();
}

/* ---- accept upload fragments and send exactly one final reply ------------ */

static void check_uploads()
{
    const auto first = frame(USB_MSG_UPLOAD, {1, 4, 0, 0, 0, 10, 20}, 6);
    assert(first == std::vector<uint8_t>({2, 1, 6, 4, 0, 0, 0, 10, 20}));
    send_bytes(usb_client, first, 1);
    assert(usb_reply.empty() && is_sample_upload_active());
    send_bytes(usb_client, {30, 40}, 1);
    assert(usb_reply == std::vector<uint8_t>({6, 2, 0}));
    expect_reply(USB_MSG_UPLOAD);
    assert(!is_sample_upload_active() && get_sample_length(6) == 4);
    const int8_t expected[] = {10, 20, 30, 40};
    assert(std::memcmp(get_sample_data(6), expected, 4) == 0);
    std::vector<uint8_t> oversized{1, 1, 2, 0, 0};
    oversized.resize(5 + 513, 16);
    const std::vector<std::vector<uint8_t>> invalid = {
        oversized,
        {9, 2, 0, 0, 0, 10, 20},
        std::vector<uint8_t>(5 + 20, 0)
    };
    for (auto payload : invalid)
    {
        if (payload[0] == 0)
        {
            payload[0] = USB_UPLOAD_SCRIPT;
            payload[1] = 20;
        }
        send_bytes(usb_client, frame(USB_MSG_UPLOAD, payload, 6), 7);
        assert(usb_reply.size() > 3 && usb_reply[0] == 6 && usb_reply[1] == 2 &&
            usb_reply[2] == 1 && usb_reply.back() == '\n');
        usb_reply.clear();
        assert(!is_sample_upload_active() && !usb_serial_buffer.faulted);
    }
    std::vector<uint8_t> maximum{USB_UPLOAD_SCRIPT, 0x14, 0x40, 0, 0};
    maximum.resize(5 + USB_UPLOAD_BYTES_MAX, 0);
    auto large = frame(USB_MSG_UPLOAD, maximum, 6);
    send_bytes(usb_client, {large.begin(), large.end() - 1}, 257);
    assert(usb_reply.empty());
    send_bytes(usb_client, {large.back()}, 1);
    assert(usb_reply.size() > 3 && usb_reply[2] == 1 && usb_reply.back() == '\n');
    usb_reply.clear();
    assert(!usb_serial_buffer.faulted);
    send_bytes(usb_client, frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 30, 40}, 6), 2);
    expect_reply(USB_MSG_UPLOAD);
    assert(get_sample_length(6) == 2 && get_sample_data(6)[0] == 30);
}

/* ---- never reply to BODY, including malformed and obsolete sessions ------ */

static void check_silent_body()
{
    const auto stale = frame(USB_MSG_BODY, {1, 2, 3}, 0, 120);
    assert(stale == std::vector<uint8_t>({1, 0, 120, 3, 0, 1, 2, 3}));
    send_bytes(usb_client, stale, 1);
    assert(usb_reply.empty() && !usb_serial_buffer.faulted);
    const std::vector<std::vector<uint8_t>> invalid = {
        frame(USB_MSG_BODY, {12}, 8, 0),
        frame(USB_MSG_BODY, {12}, 0, 255),
        frame(USB_MSG_BODY, {}, 0, 1),
        {1, 0, 1, 1, 4}
    };
    for (const auto &bytes : invalid)
    {
        send_bytes(usb_client, bytes, 1);
        assert(usb_reply.empty() && usb_serial_buffer.faulted);
        reset_usb_connection();
    }
}

/* ---- retain a full BODY message until playback frees queue space --------- */

static void check_body_backpressure()
{
    prepare_pending_voice_stream(0, 0, 1);
    std::vector<uint8_t> input;
    auto body = frame(USB_MSG_BODY, std::vector<uint8_t>(1024, 32), 0, 1);
    for (unsigned i = 0; i < 4; ++i)
    {
        input.insert(input.end(), body.begin(), body.end());
    }
    auto query = frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 16, 32}, 7);
    input.insert(input.end(), query.begin(), query.end());
    uint32_t before = get_usb_received_body_count();
    received_usb(input);
    assert(get_usb_received_body_count() == before + 3);
    assert(usb_serial_buffer.input_size == query.size());
    collect(usb_client, usb_reply);
    assert(usb_reply.empty());
    assert(activate_pending_voice_stream(0) == 0);
    skip_stream_samples(0, 1024);
    int32_t buffer[SPB];
    generate_samples(buffer);
    assert(get_usb_received_body_count() == before + 4);
    assert(get_current_stream_buffered_samples(0) == 3072);
    assert(!usb_serial_buffer.input_size);
    collect(usb_client, usb_reply);
    expect_reply(USB_MSG_UPLOAD);
    reset_voice_stream(0);
}

/* ---- check partial blocks, consecutive callbacks and ring wraparound ----- */

static void check_ring()
{
    int32_t produced = 1;
    for (unsigned i = 0; i < BUF_COUNT - 1; ++i)
    {
        for (auto &sample : audio_buffers[i])
        {
            sample = produced++;
        }
    }
    int32_t consumed = 1;
    for (unsigned cycle = 0; cycle < 20; ++cycle)
    {
        for (unsigned frames : {1u, 7u, 40u, 49u, 128u, 480u, 1024u, 96u})
        {
            std::vector<int32_t> output(frames + 2, -999);
            assert(audio_callback(nullptr, output.data() + 1, frames, nullptr, 0, nullptr) == paContinue);
            assert(output.front() == -999 && output.back() == -999);
            for (unsigned i = 1; i <= frames; ++i)
            {
                assert(output[i] == consumed++);
            }
            int pending;
            ioctl(audio_requests[0], FIONREAD, &pending);
            assert(unsigned(pending) == unsigned(consumed - 1) / SPB - unsigned(produced - 1) / SPB + BUF_COUNT - 1);
            while (pending--)
            {
                char token = 9;
                assert(read(audio_requests[0], &token, 1) == 1 && token == 0);
                for (auto &sample : audio_buffers[write_index])
                {
                    sample = produced++;
                }
                write_index = (write_index + 1) % BUF_COUNT;
            }
        }
    }
    std::vector<int32_t> drained(BUF_COUNT * SPB + 10);
    unsigned available = produced - consumed;
    audio_callback(nullptr, drained.data(), drained.size(), nullptr, 0, nullptr);
    for (unsigned i = 0; i < available; ++i)
    {
        assert(drained[i] == consumed++);
    }
    assert(std::all_of(drained.begin() + available, drained.end(), [](int32_t n) { return n == 0; }));
    settle();
    assert((write_index + BUF_COUNT - read_index) % BUF_COUNT == BUF_COUNT - 1);
}

/* ---- upload existing Berry bytecode through the simulator USB endpoint --- */

static void upload(uint8_t kind, const std::vector<uint8_t> &data)
{
    uint32_t size = data.size();
    std::vector<uint8_t> payload{kind, uint8_t(size), uint8_t(size >> 8),
        uint8_t(size >> 16), uint8_t(size >> 24)};
    payload.insert(payload.end(), data.begin(), data.end());
    send_bytes(usb_client, frame(USB_MSG_UPLOAD, payload), 7);
    expect_reply(USB_MSG_UPLOAD);
}

/* ---- send a note command using RS485 ------------------------------------ */

static void command(const std::string &text)
{
    send_bytes(rs485_client, {text.begin(), text.end()}, 3);
    assert(serial_reply.empty());
    serial_reply.clear();
}

/* ---- play and refill a callback batch through the real platform loop ----- */

static std::vector<int32_t> play(unsigned frames)
{
    std::vector<int32_t> samples(frames);
    audio_callback(nullptr, samples.data(), frames, nullptr, 0, nullptr);
    settle();
    return samples;
}

/* ---- verify uploaded envelope produces audio and completes note release -- */

static void check_program(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> program{std::istreambuf_iterator<char>(file), {}};
    assert(!program.empty());
    upload(USB_UPLOAD_SCRIPT, program);
    upload(USB_UPLOAD_ATTACK, std::vector<uint8_t>(512, 16));
    assert(is_voice_script_active(0));
    command("c:n0 on 0 69 127 @1\r");
    send_bytes(usb_client, frame(USB_MSG_BODY, std::vector<uint8_t>(1024, 16), 0, 1), 13);
    assert(usb_reply.empty());
    bool sounding = false;
    for (unsigned block = 0; block < 60; ++block)
    {
        if (get_current_stream_free_samples(0) >= 1024)
        {
            send_bytes(usb_client, frame(USB_MSG_BODY, std::vector<uint8_t>(1024, 16), 0, 1), 1029);
        }
        auto samples = play(480);
        sounding |= std::any_of(samples.begin(), samples.end(), [](int32_t n) { return n != 0; });
    }
    assert(sounding && is_voice_active(0) && get_voice_script_fault_count(0) == 0);
    command("c:n0 off\r");
    for (unsigned block = 0; block < 40; ++block)
    {
        if (is_voice_active(0) && get_current_stream_free_samples(0) >= 1024)
        {
            send_bytes(usb_client, frame(USB_MSG_BODY, std::vector<uint8_t>(1024, 16), 0, 1), 1029);
        }
        play(480);
    }
    assert(!is_voice_active(0) && get_voice_script_fault_count(0) == 0);
    auto silent = play(480);
    assert(std::all_of(silent.begin(), silent.end(), [](int32_t n) { return n == 0; }));
    const std::string restart = "c:clear\rc:n0 on 0 69 127 @22\r";
    received_serial({restart.begin(), restart.end()});
    collect(rs485_client, serial_reply);
    assert(std::string(serial_reply.begin(), serial_reply.end()) == "[C] ok:clear rt=1\r\n");
    serial_reply.clear();
    assert(get_target_stream_session(0) == 22);
    send_bytes(usb_client, frame(USB_MSG_BODY, std::vector<uint8_t>(1024, 16), 0, 22), 1029);
    assert(is_voice_active(0) && get_current_stream_session(0) == 22);

}

/* ---- exercise the actual simulator adapter with deterministic device I/O - */

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (std::string(argv[1]) == "--main-loop")
    {
        run_main_loop = true;
        return simulator_main();
    }
    open_serial_port(usb_port, usb_client, "USB");
    open_serial_port(rs485_port, rs485_client, "RS485");
    setup_select();
    init_application();
    open_audio();
    check_serial();
    check_filter_replies();
    check_pending_commands();
    check_input_capacity();
    check_uploads();
    check_silent_body();
    check_body_backpressure();
    check_ring();
    check_program(argv[1]);
    std::puts("PASS shared simulator: silent prefill, variable callbacks, partial buffers, ring wrap, independent PTYs, fragmented/combined frames, partial writes, retained input, Berry upload and note on/off");
}
