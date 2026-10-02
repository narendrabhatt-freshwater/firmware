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

#include "voicebd.h"

/*
 * RS485 carries note and status commands. One binary CDC connection carries
 * chunked BEC/ATTACK uploads and credit-controlled BODY samples.
 */

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <libserialport.h>
#if defined(__APPLE__)
// macOS exposes the serial receive latency through IOSSDATALAT.
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#if defined(__linux__)
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <future>
#include <system_error>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voice_board_detail {

// Give the USB and RS485 workers priority so refills are handled promptly.
/* ---- set streaming priority --------------------------------------------- */

static std::string prioritize_stream_thread()
{
#if defined(__APPLE__)
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase)!=KERN_SUCCESS || !timebase.numer) return {};
    thread_time_constraint_policy_data_t policy{};
    policy.period=static_cast<uint32_t>(1000000ull*timebase.denom/timebase.numer);
    policy.computation=policy.period/4u;
    policy.constraint=policy.period;
    policy.preemptible=TRUE;
    mach_port_t thread=mach_thread_self();
    (void)thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(),thread);
#elif defined(__linux__)
    // Below the usual threaded IRQ priority; equal-priority workers time-share.
    sched_param priority{};
    priority.sched_priority=20;
    int result=pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
    if (result) return "Linux streaming requires SCHED_RR priority 20: " +
        std::error_code(result,std::generic_category()).message() +
        ". Set your account's rtprio limit or the service's LimitRTPRIO to at least 20.";
#endif
    return {};
}

// Report scheduling errors before the worker starts using the connection.
/* ---- start a worker ----------------------------------------------------- */

template<class Work>
std::string start_stream_thread(std::thread& thread, Work work)
{
    std::promise<std::string> started;
    auto result=started.get_future();
    thread=std::thread([work=std::move(work), started=std::move(started)]() mutable {
        auto error=prioritize_stream_thread();
        bool ready=error.empty();
        started.set_value(std::move(error));
        if (ready) work();
    });
    auto error=result.get();
    if (!error.empty()) thread.join();
    return error;
}

/* Channel Card wire-format constants. */

constexpr unsigned kVoiceCount = voice_board_t::voice_count;
constexpr unsigned kSampleSlotCount = 248u;
constexpr unsigned kSampleRate = 48000u;
constexpr unsigned kAttackSampleCount = 512u;
constexpr unsigned kCrossfadeSampleCount = 32u;
constexpr unsigned kUsbHeaderBytes=5u;
constexpr uint8_t kUsbProtocolVersion=2u;
constexpr unsigned kUsbPayloadMax=1024u;
constexpr unsigned kPrimeSamples=998u;
enum : uint8_t { kHello=1, kBody, kUploadBegin, kUploadData, kUploadAbort, kReply };
constexpr uint8_t kSessionIdWrap = 255u;
constexpr size_t kBecHeaderBytes = 20u;
constexpr size_t kBecMaxPayload = 16384u;
/* Enough history to cover packets sent between status replies. */
constexpr size_t kPendingPacketCapacity = 64u;
constexpr unsigned kPendingWireBytes = 8u * (kUsbHeaderBytes + kUsbPayloadMax);
constexpr std::array<uint8_t, 4> kBecMagic{{0x46u, 0x57u, 0x53u, 0x43u}};

/* ---- return a successful result ----------------------------------------- */

voice_board_result_t ok(std::string message = {})
{
    return {voice_board_error_t::ok, std::move(message)};
}

/* ---- return an error ---------------------------------------------------- */

voice_board_result_t fail(voice_board_error_t code, std::string message)
{
    return {code, std::move(message)};
}

/* ---- read and write little-endian values -------------------------------- */

uint16_t read16(uint8_t const* p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t read32(uint8_t const* p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8u) |
        (static_cast<uint32_t>(p[2]) << 16u) |
        (static_cast<uint32_t>(p[3]) << 24u);
}

void write16(uint8_t* p,uint16_t n) { p[0]=static_cast<uint8_t>(n); p[1]=static_cast<uint8_t>(n>>8); }
void write32(uint8_t* p,uint32_t n) { write16(p,static_cast<uint16_t>(n)); write16(p+2,static_cast<uint16_t>(n>>16)); }

/* Shared by BODY streaming and upload/control requests. */
/* ---- frame a USB message ------------------------------------------------ */

void write_usb_header(uint8_t* out, uint8_t type, uint8_t target,
    uint8_t session, uint16_t payload_size)
{
    out[0] = type;
    out[1] = target;
    out[2] = session;
    write16(out + 3, payload_size);
}

/* poll uses -1 for an unlimited wait; both request and write deadlines bound it. */
int limit_poll_timeout(int timeout, std::chrono::steady_clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    int ms = static_cast<int>(std::max<int64_t>(0, left));
    return timeout < 0 ? ms : std::min(timeout, ms);
}

/* ---- calculate the program checksum ------------------------------------- */

uint32_t crc32(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- describe a serial error -------------------------------------------- */

std::string serial_error(std::string const& operation, enum sp_return result)
{
    std::string message(operation);
    message += " failed";
    if (result == SP_ERR_FAIL) {
        char* detail = sp_last_error_message();
        if (detail) {
            message += ": ";
            message += detail;
            sp_free_error_message(detail);
        }
    } else if (result == SP_ERR_ARG) {
        message += ": invalid argument";
    } else if (result == SP_ERR_MEM) {
        message += ": out of memory";
    } else if (result == SP_ERR_SUPP) {
        message += ": operation not supported";
    }
    return message;
}

#if defined(__linux__)
bool set_linux_serial_latency(std::string const& path, std::string& error)
{
    struct stat info{};
    if (::stat(path.c_str(),&info)<0) {
        // Non-FTDI drivers may not expose this adapter-specific setting.
        if (errno==ENOENT) return true;
        error="cannot inspect RS485 latency timer: " + path;
        return false;
    }
    auto is_one_ms=[&] {
        std::ifstream input(path);
        unsigned latency=0;
        return (input>>latency) && latency==1u;
    };
    if (is_one_ms()) return true;
    {
        std::ofstream output(path);
        output << "1\n";
        output.flush();
        if (!output) {
            error="cannot set RS485 latency timer to 1 ms: " + path +
                "; allow writes to this attribute or configure udev to set it to 1";
            return false;
        }
    }
    if (is_one_ms()) return true;
    error="RS485 latency timer did not read back as 1 ms: " + path;
    return false;
}

bool configure_linux_rs485(std::string const& name, std::string& error)
{
    // Resolve /dev/serial/by-id aliases to the actual ttyUSB device name.
    std::unique_ptr<char,decltype(&std::free)> resolved(::realpath(name.c_str(),nullptr),&std::free);
    if (!resolved) {
        error="cannot resolve RS485 device: " + name;
        return false;
    }
    std::string path(resolved.get());
    std::string tty=path.substr(path.find_last_of('/')+1);
    return set_linux_serial_latency("/sys/bus/usb-serial/devices/"+tty+"/latency_timer",error);
}
#endif

/* ---- open and use a serial port ----------------------------------------- */

class serial_port
{
private:
    struct sp_port* port_ = nullptr;
    bool opened_ = false;

public:
    serial_port() = default;
    ~serial_port() { close(); }
    serial_port(serial_port const&) = delete;
    serial_port& operator=(serial_port const&) = delete;

    bool open(std::string const& name, uint32_t baud, bool assert_dtr,
        std::string& error)
    {
        close();
        enum sp_return result = sp_get_port_by_name(name.c_str(), &port_);
        if (result != SP_OK) {
            error = serial_error("locating " + name, result);
            return false;
        }
#if defined(__linux__)
        if (!assert_dtr && !configure_linux_rs485(name,error)) {
            close();
            return false;
        }
#endif
        result = sp_open(port_, SP_MODE_READ_WRITE);
        if (result != SP_OK) {
            error = serial_error("opening " + name, result);
            close();
            return false;
        }
        opened_ = true;
        if (baud > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            sp_set_baudrate(port_, static_cast<int>(baud)) != SP_OK ||
                sp_set_bits(port_, 8) != SP_OK ||
                sp_set_parity(port_, SP_PARITY_NONE) != SP_OK ||
                sp_set_stopbits(port_, 1) != SP_OK ||
                sp_set_flowcontrol(port_, SP_FLOWCONTROL_NONE) != SP_OK) {
            error = "configuring " + name + " failed";
            close();
            return false;
        }
#if defined(__APPLE__)
        if (!assert_dtr) {
            int fd = -1;
            unsigned long latency = 1u;
            // Deliver short RS485 replies promptly instead of batching them.
            if (sp_get_port_handle(port_, &fd) != SP_OK ||
                ioctl(fd, IOSSDATALAT, &latency) < 0) {
                error = "setting low-latency RS485 reception failed";
                close();
                return false;
            }
        }
#endif
        if (assert_dtr) {
            // Toggle DTR to discard any unfinished messages from the previous connection.
            if (sp_set_dtr(port_,SP_DTR_OFF)!=SP_OK || sp_flush(port_,SP_BUF_BOTH)!=SP_OK ||
                sp_set_dtr(port_,SP_DTR_ON)!=SP_OK) {
                error="resetting CDC connection failed"; close(); return false;
            }
        }
        (void)sp_flush(port_, SP_BUF_BOTH);
        return true;
    }

    void close()
    {
        if (!port_) return;
        if (opened_) (void)sp_close(port_);
        sp_free_port(port_);
        port_ = nullptr;
        opened_ = false;
    }

    bool write(void const* data, size_t size, std::string& error)
    {
        if (!opened_) {
            error = "serial port is not open";
            return false;
        }
        enum sp_return const result = sp_blocking_write(
            port_, data, size, 3000u);
        if (result < 0 || static_cast<size_t>(result) != size) {
            error = serial_error("serial write", result);
            return false;
        }
        return true;
    }

    size_t read(void* data, size_t capacity, unsigned timeout_ms)
    {
        if (!opened_ || capacity == 0u) return 0u;
        enum sp_return const result = sp_blocking_read_next(
            port_, data, capacity, timeout_ms == 0u ? 1u : timeout_ms);
        return result > 0 ? static_cast<size_t>(result) : 0u;
    }

    int fd() const {
        int handle=-1; if (port_) (void)sp_get_port_handle(port_,&handle); return handle;
    }
    int read_nonblocking(void* data,size_t n) {
        return sp_nonblocking_read(port_,data,n);
    }
    int write_nonblocking(void const* data,size_t n) {
        return sp_nonblocking_write(port_,data,n);
    }
    void flush() { if (opened_) (void)sp_flush(port_, SP_BUF_INPUT); }
    bool is_open() const { return opened_; }

};

/* Check the BEC header and CRC before touching the card. */

/* ---- validate a compiled voice program ---------------------------------- */

voice_board_result_t read_bec(std::string const& path,
    std::vector<uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return fail(voice_board_error_t::bec_error, "cannot open BEC: " + path);
    bytes.assign(std::istreambuf_iterator<char>(input), {});
    if (bytes.size() < kBecHeaderBytes ||
        bytes.size() > kBecHeaderBytes + kBecMaxPayload ||
            !std::equal(kBecMagic.begin(), kBecMagic.end(), bytes.begin()) ||
            read16(bytes.data() + 4u) != 1u || bytes[6] != 1u ||
            bytes[7] != 0x03u || read16(bytes.data() + 8u) != 2u ||
            read16(bytes.data() + 10u) != kBecHeaderBytes ||
            read32(bytes.data() + 12u) != bytes.size() - kBecHeaderBytes ||
            read32(bytes.data() + 16u) !=
                crc32(bytes.data() + kBecHeaderBytes,
                    bytes.size() - kBecHeaderBytes)) {
        return fail(voice_board_error_t::bec_error,
            "invalid or incompatible Channel BEC");
    }
    return ok();
}

/* ---- decode voice buffer status ----------------------------------------- */

struct card_status
{
    uint8_t active_voice_mask = 0u;
    uint8_t pending_voice_mask = 0u;
    uint16_t buffer_capacity = 0u;
    uint16_t status_sequence = 0u;
    uint16_t last_usb_sequence = 0u;
    uint8_t usb_age_ms = 255u;
    std::array<uint8_t, kVoiceCount> session_id{};
    std::array<uint32_t, kVoiceCount> remaining_us{};
    std::array<uint16_t, kVoiceCount> free_samples{};
    std::array<uint16_t, kVoiceCount> refill_samples_5ms{};
};

bool parse_voice_queue_status(std::vector<uint8_t> const& raw,
    card_status& status)
{
    for (size_t start = 0; start + 61u <= raw.size(); ++start) {
        uint8_t const* p = raw.data() + start;
        if (p[0] != 0xA5u || p[1] != 0x5Au || p[2] != 0x43u ||
            p[3] != 0x0Cu || p[60] != '\n') continue;
        status.active_voice_mask = p[4];
        status.pending_voice_mask = p[5];
        status.buffer_capacity = read16(p + 6u);
        status.status_sequence = p[8];
        status.usb_age_ms = p[9];
        status.last_usb_sequence = read16(p + 10u);
        if (status.buffer_capacity == 0u || status.buffer_capacity > 8191u) return false;
        for (size_t i = 0; i < kVoiceCount; ++i) {
            size_t const at = 12u + i * 6u;
            status.session_id[i] = p[at];
            status.free_samples[i] = uint16_t(p[at + 1u]) |
                (uint16_t(p[at + 2u] & 0x1Fu) << 8u);
            status.refill_samples_5ms[i] = uint16_t(p[at + 2u] >> 5u) |
                (uint16_t(p[at + 3u]) << 3u) | (uint16_t(p[at + 4u] & 1u) << 11u);
            if (status.refill_samples_5ms[i] > 3840u) return false;
            uint16_t const duration = uint16_t(p[at + 4u] >> 1u) |
                (uint16_t(p[at + 5u]) << 7u);
            status.remaining_us[i] = static_cast<uint32_t>(duration) * 100u;
            if (status.free_samples[i] > status.buffer_capacity ||
                (((status.active_voice_mask | status.pending_voice_mask) &
                  (1u << i)) != 0u && status.session_id[i] == 0xFFu))
                return false;
        }
        return true;
    }
    return false;
}

/* ---- exchange commands with the card ------------------------------------ */

class rs485_link
{
private:
    serial_port port_;
    std::mutex mutex_;
    uint32_t baud_ = 0u;
    std::chrono::steady_clock::time_point transmitted_at_{};

    voice_board_result_t write(std::string const& wire)
    {
        std::string error;
        if (!port_.write(wire.data(), wire.size(), error))
            return fail(voice_board_error_t::io_error, error);
        transmitted_at_ = std::max(transmitted_at_, std::chrono::steady_clock::now()) +
            std::chrono::microseconds((wire.size() * 10000000ull + baud_ - 1u) / baud_);
        return ok();
    }

public:
    voice_board_result_t open(std::string const& path, uint32_t baud)
    {
        std::string error;
        if (!port_.open(path, baud, false, error))
            return fail(voice_board_error_t::io_error, error);
        baud_ = baud;
        transmitted_at_ = std::chrono::steady_clock::now();
        return ok();
    }

    void close() { port_.close(); }
    bool is_open() const { return port_.is_open(); }

    voice_board_result_t send_event(std::string const& body)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return write("c:" + body + "\r");
    }

    voice_board_result_t command(std::string const& body,
        std::vector<uint8_t>* binary = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string const wire = (body.size() > 1u && body[1] == ':')
            ? body + "\r" : "c:" + body + "\r";
        port_.flush();
        auto sent = write(wire);
        if (!sent) return sent;
        std::vector<uint8_t> reply;
        auto const stop_waiting_at = transmitted_at_ +
            std::chrono::milliseconds(5);
        std::array<uint8_t, 256> bytes{};
        while (std::chrono::steady_clock::now() < stop_waiting_at) {
            size_t const count = port_.read(
                bytes.data(), bytes.size(), 1u);
            reply.insert(reply.end(), bytes.begin(), bytes.begin() + count);
            if (body == "vq") {
                std::string const text(reply.begin(), reply.end());
                if (text.find("err:syntax") != std::string::npos)
                    return fail(voice_board_error_t::bad_reply,
                        "Channel Card firmware must support the 61-byte vq reply");
                for (size_t i = 0; i + 4u <= reply.size(); ++i) {
                    if (reply[i] != 0xA5u || reply[i + 1u] != 0x5Au ||
                        reply[i + 2u] != 0x43u) continue;
                    if (reply[i + 3u] != 0x0Cu)
                        return fail(voice_board_error_t::bad_reply,
                            "invalid voice status reply header");
                    break;
                }
                card_status status;
                if (parse_voice_queue_status(reply, status)) {
                    if (binary) *binary = std::move(reply);
                    return ok();
                }
            } else {
                std::string const text(reply.begin(), reply.end());
                size_t const tag = text.find('[');
                size_t const end = text.find('\n', tag);
                if (tag != std::string::npos && end != std::string::npos) {
                    size_t const length = end - tag -
                        (end > tag && text[end - 1u] == '\r' ? 1u : 0u);
                    std::string const line = text.substr(tag, length);
                    if (line.find("err:") != std::string::npos)
                        return fail(voice_board_error_t::bad_reply,
                            line + " (command: " + body + ")");
                    return ok(line);
                }
            }
        }
        return fail(voice_board_error_t::timeout,
            "RS485 timeout waiting for Channel Card reply to: " + body);
    }

};

/* ---- track samples and voice demand ------------------------------------- */

struct sample_data
{
    // Retain the prefix too: a replacement may move BODY origin past a live cursor.
    std::unique_ptr<int16_t[]> pcm;
    size_t pcm_size = 0u;
    bool loaded = false;
};

struct voice_data
{
    bool active = false;
    unsigned initial_samples_left = 0u;
    bool note_seen_in_status = false;
    uint8_t session_id = 0u;
    uint16_t sample_id = 0u;
    size_t source_position = 0u;
    size_t stream_origin = 0u;
    unsigned available_sample_slots = 0u;
    bool pending = true;
    unsigned refill_samples_5ms = 0u;
    double refill_balance = 0;
    std::chrono::steady_clock::time_point refill_at{}, forecast_until{};
    std::chrono::steady_clock::time_point deadline{};
};

/* BODY blocks remain charged until a vq snapshot acknowledges processing them. */
struct pending_packet {
    uint16_t sequence;
    uint8_t voice, session;
    uint16_t samples;
};
struct stream_state {
    std::mutex mutex;
    std::array<sample_data, kSampleSlotCount> samples;
    std::array<voice_data, kVoiceCount> voices;
    std::array<pending_packet, kPendingPacketCapacity> pending_packets{};
    size_t first_pending_packet=0, pending_packet_count=0;
    uint16_t next_sequence=0;
    uint8_t next_voice=0, last_status_sequence=0;
    bool sequence_ready=false, status_ready=false;
    unsigned buffer_capacity=0;
    std::chrono::steady_clock::time_point last_status_at{};

    void apply_status(card_status const& status,
        std::chrono::steady_clock::time_point requested_at,
        std::chrono::steady_clock::time_point received_at=std::chrono::steady_clock::now())
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (status_ready && requested_at-last_status_at<std::chrono::milliseconds(500)) {
            uint8_t distance=static_cast<uint8_t>(status.status_sequence-last_status_sequence);
            if (!distance || distance>=0x80u) return;
        }
        last_status_sequence=static_cast<uint8_t>(status.status_sequence);
        status_ready=true; last_status_at=requested_at; buffer_capacity=status.buffer_capacity;
        if (!sequence_ready) {
            next_sequence=static_cast<uint16_t>(status.last_usb_sequence+1u);
            sequence_ready=true;
        }
        while (pending_packet_count) {
            auto const& front=pending_packets[first_pending_packet];
            if (static_cast<uint16_t>(status.last_usb_sequence-front.sequence)>=0x8000u) break;
            first_pending_packet=(first_pending_packet+1u)%kPendingPacketCapacity;
            --pending_packet_count;
        }
        std::array<unsigned,kVoiceCount> in_flight{};
        for (size_t i=0;i<pending_packet_count;++i) {
            auto const& sent=pending_packets[(first_pending_packet+i)%kPendingPacketCapacity];
            in_flight[sent.voice]+=sent.samples;
        }
        uint8_t live=static_cast<uint8_t>(status.active_voice_mask|status.pending_voice_mask);
        for (uint8_t v=0;v<kVoiceCount;++v) {
            auto& voice=voices[v];
            voice.available_sample_slots=status.free_samples[v]>in_flight[v]
                ? status.free_samples[v]-in_flight[v] : 0;
            voice.refill_balance=static_cast<double>(status.free_samples[v])-in_flight[v];
            voice.refill_at=received_at;
            voice.forecast_until=received_at+std::chrono::milliseconds(5);
            if (!voice.active) continue;
            if (!(live&(1u<<v)) || status.session_id[v]!=voice.session_id) {
                voice.available_sample_slots=0; voice.refill_samples_5ms=0;
                if (voice.note_seen_in_status && !(live&(1u<<v))) voice.active=false;
                continue;
            }
            voice.note_seen_in_status=true;
            voice.pending=(status.pending_voice_mask&(1u<<v))!=0;
            voice.deadline=requested_at+std::chrono::microseconds(status.remaining_us[v]);
            voice.refill_samples_5ms=voice.pending ? 0 : status.refill_samples_5ms[v];
        }
    }

    unsigned poll_interval_ms() {
        std::lock_guard<std::mutex> lock(mutex);
        unsigned demand=0;
        for (auto const& voice:voices)
            if (voice.active) demand=std::max(demand,voice.refill_samples_5ms);
        // Four polls per ring of source data at high pitch. Default remains
        // 5 ms; the card supplies demand, so host never computes pitch.
        return demand ? std::clamp(buffer_capacity*5u/(4u*demand),1u,5u) : 5u;
    }

    int refill_wake_ms() {
        std::lock_guard<std::mutex> lock(mutex);
        auto now=std::chrono::steady_clock::now();
        for (auto const& v:voices)
            if(v.active && v.refill_samples_5ms && now<v.forecast_until) return 1;
        return -1;
    }

    bool urgent() {
        std::lock_guard<std::mutex> lock(mutex);
        auto now=std::chrono::steady_clock::now();
        for (auto const& v:voices)
            if (v.active && v.available_sample_slots &&
                (v.initial_samples_left || (!v.pending && v.deadline<=now+std::chrono::milliseconds(10)))) return true;
        return false;
    }

    /* Called only by the USB worker. No allocation, waits or serial I/O under
     * this lock. Credits include the partially written block immediately. */
    size_t make_block(uint8_t *out,
                      std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now(),
                      size_t capacity=kUsbHeaderBytes+kUsbPayloadMax) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!sequence_ready || pending_packet_count>=kPendingPacketCapacity || capacity<=kUsbHeaderBytes) return 0;
        std::array<unsigned,kVoiceCount> queued{}, all_queued{};
        unsigned wire_bytes=0;
        for (size_t i=0;i<pending_packet_count;++i) {
            auto const& packet=pending_packets[(first_pending_packet+i)%kPendingPacketCapacity];
            wire_bytes+=kUsbHeaderBytes+packet.samples;
            all_queued[packet.voice]+=packet.samples;
            if (packet.session==voices[packet.voice].session_id) queued[packet.voice]+=packet.samples;
        }
        if (wire_bytes+kUsbHeaderBytes>=kPendingWireBytes) return 0;
        // Forecast demand for five milliseconds from this status reply, keeping
        // two milliseconds of samples in reserve.
        for (uint8_t i=0;i<kVoiceCount;++i) {
            auto& v=voices[i];
            if(!v.active || !v.refill_samples_5ms)continue;
            auto until=std::min(now,v.forecast_until);
            double ms=std::chrono::duration<double,std::milli>(until-v.refill_at).count();
            if(ms>0) {
                double ceiling=static_cast<double>(buffer_capacity)-all_queued[i];
                v.refill_balance=std::min(ceiling,v.refill_balance+ms*v.refill_samples_5ms/5.0);
                v.refill_at=until;
            }
            double allowance=v.refill_balance-std::ceil(2.0*v.refill_samples_5ms/5.0);
            v.available_sample_slots=allowance>0 ? static_cast<unsigned>(allowance) : 0;
        }
        uint8_t chosen=kVoiceCount;
        auto rank=[&](voice_data const& v) {
            if (!v.pending && v.deadline<=now+std::chrono::milliseconds(3)) return 0;
            if (v.initial_samples_left) return 1;
            return 2;
        };
        for (uint8_t i=0;i<kVoiceCount;++i) {
            uint8_t v=static_cast<uint8_t>((next_voice+i)%kVoiceCount);
            auto const& candidate=voices[v];
            if (!candidate.active || !candidate.note_seen_in_status ||
                !candidate.available_sample_slots || !samples[candidate.sample_id].pcm_size) continue;
            unsigned batch=std::min(candidate.refill_samples_5ms,kUsbPayloadMax);
            if(!candidate.initial_samples_left && batch && candidate.available_sample_slots<batch &&
               candidate.deadline>now+std::chrono::milliseconds(3)) continue;
            if (chosen==kVoiceCount || rank(candidate)<rank(voices[chosen]) ||
                (rank(candidate)==rank(voices[chosen]) &&
                 (queued[v]<queued[chosen] || (queued[v]==queued[chosen] && !candidate.pending &&
                  candidate.deadline<voices[chosen].deadline)))) chosen=v;
        }
        if (chosen==kVoiceCount) return 0;
        auto& v=voices[chosen];
        auto const& sample=samples[v.sample_id];
        unsigned count=std::min({v.available_sample_slots,kUsbPayloadMax,
            kPendingWireBytes-wire_bytes-kUsbHeaderBytes, static_cast<unsigned>(capacity-kUsbHeaderBytes)});
        if (v.initial_samples_left) count=std::min(count,v.initial_samples_left);
        write_usb_header(out, kBody, chosen, v.session_id, static_cast<uint16_t>(count));
        for (unsigned i=0;i<count;++i)
            out[kUsbHeaderBytes+i]=v.source_position<sample.pcm_size
                ? static_cast<uint8_t>(sample.pcm[v.source_position++]>>8) : 0u;
        pending_packets[(first_pending_packet+pending_packet_count)%kPendingPacketCapacity]=
            {next_sequence,chosen,v.session_id,static_cast<uint16_t>(count)};
        ++pending_packet_count; ++next_sequence;
        v.available_sample_slots-=count;
        v.refill_balance-=count;
        v.initial_samples_left-=std::min(v.initial_samples_left,count);
        next_voice=static_cast<uint8_t>((chosen+1u)%kVoiceCount);
        return kUsbHeaderBytes+count;
    }
};

// One worker handles all USB reads and writes. MIDI, status and uploads wake it
// when there is work; otherwise it waits for the port or the next refill.
/* ---- stream BODY blocks and uploads ------------------------------------- */

class usb_link {
    serial_port port_;
    stream_state *stream_=nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false}, streaming_{false};
    int wake_[2]={-1,-1};
    std::mutex mutex_, request_mutex_;
    std::condition_variable done_;
    std::vector<uint8_t> request_, response_;
    uint8_t request_type_=0;
    bool request_pending_=false, request_sent_=false, request_done_=false;
    std::string error_;
    std::chrono::steady_clock::time_point request_deadline_;

    void set_error(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_=std::move(message); running_=false; done_.notify_all();
    }
    size_t prepare_output(std::array<uint8_t,kPendingWireBytes>& output) {
        size_t size=0;
        bool have_request;
        { std::lock_guard<std::mutex> lock(mutex_); have_request=request_pending_ && !request_sent_; }
        if (streaming_) {
            while (output.size()-size>kUsbHeaderBytes && (!have_request || stream_->urgent())) {
                size_t n=stream_->make_block(output.data()+size,std::chrono::steady_clock::now(),output.size()-size);
                if(!n)break;
                size+=n;
            }
        }
        if (!size && have_request) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::copy(request_.begin(),request_.end(),output.begin());
            size=request_.size(); request_sent_=true;
        }
        if (!size && streaming_) size=stream_->make_block(output.data());
        return size;
    }

    bool receive_reply(std::array<uint8_t,kUsbHeaderBytes+kUsbPayloadMax>& input,
        size_t& received, size_t& needed) {
        int n=port_.read_nonblocking(input.data()+received,needed-received);
        if (n<0) { set_error("CDC read failed"); return false; }
        received+=static_cast<size_t>(n);
        if (received==kUsbHeaderBytes && needed==kUsbHeaderBytes) {
            unsigned payload=read16(input.data()+3);
            if (input[0]!=kReply || !payload || payload>kUsbPayloadMax) { set_error("invalid CDC reply framing"); return false; }
            needed=kUsbHeaderBytes+payload;
        }
        if (received==needed) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!request_pending_ || !request_sent_ || input[2]!=request_type_ ||
                input[1]!=request_[1] || input[kUsbHeaderBytes]>1 ||
                (input[kUsbHeaderBytes]==0 && needed!=kUsbHeaderBytes+(request_type_==kHello ? 14u : 1u))) {
                error_="unexpected CDC reply; BODY rejected or protocol lost"; running_=false; done_.notify_all(); return false;
            }
            response_.assign(input.begin()+kUsbHeaderBytes,input.begin()+needed);
            request_done_=true; request_pending_=false; done_.notify_all();
            received=0; needed=kUsbHeaderBytes;
        }
        return true;
    }

    void run() {
        std::array<uint8_t,kPendingWireBytes> output{};
        std::array<uint8_t,kUsbHeaderBytes+kUsbPayloadMax> input{};
        size_t size=0, sent=0, received=0, needed=kUsbHeaderBytes;
        auto write_deadline=std::chrono::steady_clock::now();
        while (running_) {
            if (sent==size) {
                sent=0;
                size=prepare_output(output);
                write_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            }
            int timeout=streaming_ ? stream_->refill_wake_ms() : -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (request_pending_)
                    timeout=limit_poll_timeout(timeout, request_deadline_);
            }
            if (size>sent) timeout=limit_poll_timeout(timeout, write_deadline);
            pollfd fds[2]={{port_.fd(),static_cast<short>(POLLIN|(size>sent ? POLLOUT : 0)),0},{wake_[0],POLLIN,0}};
            int ready=::poll(fds,2,timeout);
            if (ready<0) { if (errno==EINTR) continue; set_error("CDC poll failed"); break; }
            if (!running_) break;
            if (fds[1].revents&POLLIN) { uint8_t wake_bytes[64]; while (::read(wake_[0],wake_bytes,sizeof wake_bytes)>0) {} }
            if (fds[0].revents&(POLLERR|POLLHUP|POLLNVAL)) { set_error("CDC disconnected"); break; }
            if (fds[0].revents&POLLIN) {
                if (!receive_reply(input, received, needed)) break;
            }
            if (size>sent && (fds[0].revents&POLLOUT)) {
                int n=port_.write_nonblocking(output.data()+sent,size-sent);
                if (n<0) { set_error("CDC write failed"); break; }
                sent+=static_cast<size_t>(n);
            }
            /* Deadlines also apply when the fd is continuously ready. */
            if (size>sent && std::chrono::steady_clock::now()>=write_deadline) { set_error("CDC write stalled"); break; }
            bool expired;
            { std::lock_guard<std::mutex> lock(mutex_); expired=request_pending_ && std::chrono::steady_clock::now()>=request_deadline_; }
            if (expired) { set_error("CDC reply timed out; reopen connection"); break; }
        }
    }
public:
    ~usb_link() { close(); }
    bool good() const { return running_; }
    void wake() {
        uint8_t b=1;
        if (wake_[1]>=0) { auto ignored=::write(wake_[1],&b,1); (void)ignored; }
    }
    void enable_streaming() { streaming_=true; wake(); }
    voice_board_result_t open(std::string const& path,stream_state& stream) {
        close(); std::string error;
        if (!port_.open(path,115200,true,error)) return fail(voice_board_error_t::io_error,error);
        return start_worker(stream);
    }
private:
    voice_board_result_t start_worker(stream_state& stream) {
        if (::pipe(wake_)<0) { port_.close(); return fail(voice_board_error_t::io_error,"CDC wake pipe failed"); }
        for (int fd:wake_) {
            if (fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
                close(); return fail(voice_board_error_t::io_error,"CDC wake pipe setup failed");
            }
        }
        stream_=&stream; error_.clear(); request_pending_=request_sent_=request_done_=false;
        running_=true;
        auto scheduling_error=start_stream_thread(worker_,[this]{run();});
        if (!scheduling_error.empty()) {
            close();
            return fail(voice_board_error_t::io_error,scheduling_error);
        }
        std::vector<uint8_t> caps;
        auto result=request(kHello,0,{kUsbProtocolVersion},&caps);
        if (result && (caps.size()!=14 || caps[1]!=kUsbProtocolVersion || caps[2]!=kVoiceCount || caps[3]!=1 ||
            read32(caps.data()+4)!=kSampleRate || read16(caps.data()+8)!=kUsbPayloadMax ||
            read16(caps.data()+10)!=kPrimeSamples)) result=fail(voice_board_error_t::bad_reply,"incompatible CDC firmware capabilities");
        if (result) {
            // HELLO reports the count after the old connection was cleared. The
            // last RS485 reply may predate some completed BODY blocks.
            std::lock_guard<std::mutex> lock(stream.mutex);
            stream.first_pending_packet=stream.pending_packet_count=0;
            stream.next_sequence=static_cast<uint16_t>(read16(caps.data()+12)+1u);
            stream.sequence_ready=true;
        } else close();
        return result;
    }
public:
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_=false; streaming_=false; done_.notify_all();
        }
        wake();
        if (worker_.joinable()) worker_.join();
        port_.close();
        for (int& fd:wake_) { if (fd>=0) ::close(fd); fd=-1; }
    }
    voice_board_result_t request(uint8_t type,uint8_t target,std::vector<uint8_t> const& payload,
                                 std::vector<uint8_t>* response=nullptr) {
        std::lock_guard<std::mutex> serial(request_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) return fail(voice_board_error_t::io_error,error_.empty() ? "CDC is closed" : error_);
        request_.assign(kUsbHeaderBytes+payload.size(),0);
        write_usb_header(request_.data(), type, target, 0,
            static_cast<uint16_t>(payload.size()));
        request_type_=type;
        std::copy(payload.begin(),payload.end(),request_.begin()+kUsbHeaderBytes);
        response_.clear(); request_done_=request_sent_=false; request_pending_=true;
        request_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        wake(); done_.wait(lock,[this]{return request_done_ || !running_;});
        if (!request_done_) return fail(voice_board_error_t::io_error,error_.empty() ? "CDC closed during request" : error_);
        if (response) *response=response_;
        if (response_[0]) return fail(voice_board_error_t::bad_reply,std::string(response_.begin()+1,response_.end()));
        return ok();
    }
    voice_board_result_t upload(uint8_t kind,uint8_t target,uint8_t const* data,size_t size) {
        std::vector<uint8_t> begin(5); begin[0]=kind; write32(begin.data()+1,static_cast<uint32_t>(size));
        auto result=request(kUploadBegin,target,begin);
        if (!result) return result;
        for (size_t offset=0;offset<size;) {
            size_t n=std::min<size_t>(512,size-offset);
            std::vector<uint8_t> part(4+n); write32(part.data(),static_cast<uint32_t>(offset));
            std::copy_n(data+offset,n,part.data()+4);
            result=request(kUploadData,target,part);
            if (!result) { if (running_) (void)request(kUploadAbort,target,{}); return result; }
            offset+=n;
        }
        return ok();
    }
};

/* ---- manage the sound-board connection ---------------------------------- */

struct device_context
{
    voice_board_config_t config;
    std::string rs485_name;
    rs485_link rs485;
    usb_link usb;
    stream_state stream;
    std::atomic<bool> connected{false};
    std::atomic<bool> worker_running{false};
    std::thread worker;
    std::mutex upload_mutex;
    std::mutex control_mutex;
    std::condition_variable control_changed;
    std::atomic<unsigned> waiting_commands{0u};
    std::chrono::steady_clock::time_point next_poll = std::chrono::steady_clock::now();
    std::atomic<voice_board_error_t> event_error{voice_board_error_t::ok};

    void record_error(voice_board_result_t result) {
        if (result || event_error.load() != voice_board_error_t::ok) return;
        event_error.store(result.code, std::memory_order_release);
        (void)rs485.command("clear");
        std::lock_guard<std::mutex> lock(stream.mutex);
        for (auto& voice : stream.voices) voice.active = false;
    }

    struct control_turn {
        device_context* state;
        std::unique_lock<std::mutex> lock;
        explicit control_turn(device_context* s)
            : state(s) {
            ++state->waiting_commands;
            lock = std::unique_lock<std::mutex>(state->control_mutex);
        }
        ~control_turn() {
            --state->waiting_commands;
            state->control_changed.notify_all();
        }
    };

    ~device_context() { shutdown(); }

    void shutdown()
    {
        std::lock_guard<std::mutex> upload_lock(upload_mutex);
        bool was_connected = connected.exchange(false);
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            worker_running.store(false);
        }
        control_changed.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
            worker.join();
        usb.close();
        if (rs485.is_open()) {
            if (was_connected) (void)rs485.command("clear");
            rs485.close();
        }
        {
            std::lock_guard<std::mutex> lock(stream.mutex);
            for (auto& voice : stream.voices) voice = {};
            stream.first_pending_packet = 0u;
            stream.pending_packet_count = 0u;
            stream.sequence_ready = false;
            stream.status_ready = false;
        }
        connected.store(false);
    }

    // Caller holds a control turn, so note-on cannot intervene before note-off.
    voice_board_result_t replace_sample(uint16_t id, sample_data& replacement)
    {
        {
            std::lock_guard<std::mutex> lock(stream.mutex);
            std::swap(stream.samples[id], replacement);
        }
        replacement.pcm.reset(); // Free old PCM outside the stream lock.
        auto result = ok();
        for (uint8_t i = 0; i < kVoiceCount; ++i) {
            std::unique_lock<std::mutex> lock(stream.mutex);
            auto& voice = stream.voices[i];
            if (!voice.active || voice.sample_id != id ||
                    voice.source_position < stream.samples[id].pcm_size) continue;
            voice.active = false;
            lock.unlock();
            auto off = rs485.send_event("n" + std::to_string(i) + " off");
            if (!off && result) {
                result = off;
                result.message = "sample committed; replacement note-off failed: " + off.message;
            }
        }
        return result;
    }

    voice_board_result_t query_status()
    {
        auto const requested_at = std::chrono::steady_clock::now();
        std::vector<uint8_t> raw;
        auto result = rs485.command("vq", &raw);
        if (result) {
            card_status status;
            if (parse_voice_queue_status(raw, status)) {
                stream.apply_status(status, requested_at);
            } else result = fail(voice_board_error_t::bad_reply, "invalid voice status reply");
            usb.wake();
        }
        return result;
    }

    void advance_poll(std::chrono::steady_clock::time_point requested_at =
        std::chrono::steady_clock::now())
    {
        next_poll = requested_at + std::chrono::milliseconds(stream.poll_interval_ms());
    }

    void poll()
    {
        std::unique_lock<std::mutex> lock(control_mutex);
        while (worker_running.load()) {
            if (waiting_commands.load() != 0u) {
                control_changed.wait(lock, [this] {
                    return !worker_running.load() || waiting_commands.load() == 0u;
                });
                continue;
            }
            if (event_error.load() != voice_board_error_t::ok) {
                control_changed.wait(lock, [this] { return !worker_running.load(); });
                continue;
            }
            if (std::chrono::steady_clock::now() < next_poll) {
                control_changed.wait_until(lock, next_poll);
                continue;
            }
            auto const requested_at = std::chrono::steady_clock::now();
            record_error(query_status());
            advance_poll(requested_at);
        }
    }

};

device_context* create() { return new (std::nothrow) device_context; }

void destroy(device_context* state)
{
    delete state;
}

voice_board_result_t upload_script(device_context* state, uint8_t voice_id,
    std::vector<uint8_t> const& bec)
{
    // Firmware rejects program replacement while the voice is active.
    auto result = state->usb.upload(3,voice_id,bec.data(),bec.size());
    if (!result) {
        result.code = voice_board_error_t::bec_error;
        result.message = "voice " + std::to_string(voice_id) +
            " BEC upload failed: " + result.message;
    }
    return result;
}

/* ---- initialize the sound board ----------------------------------------- */

voice_board_result_t open_device(device_context* state,
    voice_board_config_t const& config)
{
    if (!state) return fail(voice_board_error_t::io_error,
        "voice board state is unavailable");
    if (state->connected.load()) {
        if (state->usb.good() && state->event_error.load() == voice_board_error_t::ok)
            return ok("already connected");
        state->shutdown();
    }
    /* Read the program first so a bad file cannot disturb the card. */
    std::vector<uint8_t> bec;
    auto result = read_bec(config.bec_file, bec);
    if (!result) return result;
    state->config = config;
    state->rs485_name = config.rs485_port;
    result = state->rs485.open(state->rs485_name, config.rs485_baud);
    if (!result) return result;
    result = state->rs485.command("clear");
    if (!result) {
        state->shutdown();
        return result;
    }
    result=state->usb.open(config.usb_port,state->stream);
    if (!result) { state->shutdown(); return result; }
    // Read fresh buffer space after HELLO establishes the BODY count.
    result = state->query_status();
    if (!result) { state->shutdown(); return result; }
    for (uint8_t voice = 0u; voice < kVoiceCount; ++voice) {
        result = upload_script(state, voice, bec);
        if (!result) {
            state->shutdown();
            return result;
        }
    }
    result = state->rs485.command(
        "g 1 " + std::to_string(config.initial_attenuation_db));
    if (!result) {
        state->shutdown();
        result.message = "Channel gain setup failed: " + result.message;
        return result;
    }
    state->usb.enable_streaming();
    state->event_error.store(voice_board_error_t::ok);
    state->connected.store(true);
    state->next_poll = std::chrono::steady_clock::now();
    state->worker_running.store(true);
    auto scheduling_error=start_stream_thread(state->worker,[state] { state->poll(); });
    if (!scheduling_error.empty()) {
        state->shutdown();
        return fail(voice_board_error_t::io_error,scheduling_error);
    }
    return ok("Channel Card connected; BEC loaded into voices 0-7");
}

voice_board_result_t close_device(device_context* state)
{
    if (state) state->shutdown();
    return ok("voice board disconnected");
}

bool device_is_open(device_context const* state)
{
    return state && state->connected.load() && state->usb.good() &&
        state->event_error.load() == voice_board_error_t::ok;
}

voice_board_result_t load_script(device_context* state, uint8_t voice_id,
    std::string const& path)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    std::vector<uint8_t> bec;
    auto result = read_bec(path, bec);
    if (!result) return result;
    std::lock_guard<std::mutex> upload_lock(state->upload_mutex);
    return upload_script(state, voice_id, bec);
}

/* ---- replace a loaded sample -------------------------------------------- */

voice_board_result_t load_sample(device_context* state, uint16_t sample_id,
    std::vector<int16_t> const& pcm,
    double /*root_pitch_hz*/)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    if (pcm.empty())
        return fail(voice_board_error_t::sample_error, "sample PCM is empty");
    size_t const attack_size = std::min<size_t>(kAttackSampleCount, pcm.size());
    std::array<int8_t, kAttackSampleCount> attack{};
    for (size_t i = 0; i < attack_size; ++i)
        attack[i] = static_cast<int8_t>(pcm[i] >> 8);
    sample_data replacement;
    replacement.pcm_size = pcm.size();
    replacement.loaded = true;
    replacement.pcm.reset(new int16_t[pcm.size()]);
    std::copy(pcm.begin(), pcm.end(), replacement.pcm.get());

    // Upload transactions share CDC with BODY but never hold up vq or MIDI.
    std::lock_guard<std::mutex> upload_lock(state->upload_mutex);
    auto result = state->usb.upload(1,static_cast<uint8_t>(sample_id),
        reinterpret_cast<uint8_t const*>(attack.data()),attack_size);
    if (!result) {
        result.code = voice_board_error_t::sample_error;
        result.message = "attack upload failed; old BODY retained: " +
            result.message;
        return result;
    }

    device_context::control_turn turn(state);
    result = state->replace_sample(sample_id, replacement);
    if (!result) return result;
    return ok("sample " + std::to_string(sample_id) + " loaded");
}

/* ---- start a voice ------------------------------------------------------ */

voice_board_result_t note_on(device_context* state, uint8_t voice,
    uint16_t sample, uint8_t key, uint8_t velocity)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected, "voice board is not connected");
    device_context::control_turn turn(state);
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected, "voice board is not connected");
    uint8_t session;
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        if (!state->stream.samples[sample].loaded)
            return fail(voice_board_error_t::sample_error,
                "sample " + std::to_string(sample) + " is not loaded");
        auto& slot = state->stream.voices[voice];
        session = static_cast<uint8_t>((slot.session_id + 1u) % kSessionIdWrap);
        slot = {};
        slot.active = true;
        slot.initial_samples_left = kPrimeSamples;
        slot.session_id = session;
        slot.sample_id = sample;
        size_t attack = std::min<size_t>(kAttackSampleCount, state->stream.samples[sample].pcm_size);
        slot.source_position = attack - std::min<size_t>(kCrossfadeSampleCount, attack);
        slot.stream_origin = slot.source_position;
    }
    // Silent note event: write it now, without appending vq or reading an ACK.
    auto result = state->rs485.send_event("n" + std::to_string(voice) + " on " +
        std::to_string(sample) + " " + std::to_string(key) + " " +
        std::to_string(velocity) + " @" + std::to_string(session));
    if (!result) {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        state->stream.voices[voice].active = false;
    }
    return result;
}

/* ---- release a voice ---------------------------------------------------- */

voice_board_result_t note_off(device_context* state, uint8_t voice)
{
    // Keep note-off available even if USB streaming or status polling failed.
    if (!state || !state->connected.load() || !state->rs485.is_open())
        return fail(voice_board_error_t::not_connected, "voice board is not connected");
    device_context::control_turn turn(state);
    auto result = state->rs485.send_event("n" + std::to_string(voice) + " off");
    if (result) {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        auto& slot = state->stream.voices[voice];
        if (!slot.note_seen_in_status) slot.active = false;
    }
    return result;
}

/* ---- stop all voices ---------------------------------------------------- */

voice_board_result_t all_notes_off(device_context* state)
{
    if (!state || !state->connected.load() || !state->rs485.is_open())
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    device_context::control_turn turn(state);
    auto const result = state->rs485.command("clear");
    if (result) {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        for (auto& voice : state->stream.voices) {
            auto const session = voice.session_id;
            voice = {};
            voice.session_id = session;
        }
    }
    return result;
}

voice_board_result_t set_attenuation(device_context* state,
    uint8_t attenuation)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    return state->rs485.command("g 1 " + std::to_string(attenuation));
}

} // namespace voice_board_detail

/* ---- expose the voice-board interface ----------------------------------- */

struct voice_board_t::impl_t
{
    voice_board_detail::device_context* context = voice_board_detail::create();
    ~impl_t() { voice_board_detail::destroy(context); }
};

namespace {

voice_board_result_t invalid_argument(std::string const& message)
{
    return {voice_board_error_t::invalid_argument, message};
}

voice_board_result_t unavailable()
{
    return {voice_board_error_t::io_error,
        "voice board implementation is unavailable"};
}

} // namespace

voice_board_t::voice_board_t() : impl_(std::make_unique<impl_t>()) {}
voice_board_t::~voice_board_t() = default;
voice_board_t::voice_board_t(voice_board_t&&) noexcept = default;
voice_board_t& voice_board_t::operator=(voice_board_t&&) noexcept = default;

voice_board_result_t voice_board_t::open(voice_board_config_t const& config)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (config.bec_file.empty())
        return invalid_argument("BEC file is required");
    if (config.rs485_port.empty())
        return invalid_argument("RS485 port name is required");
    if (config.usb_port.empty())
        return invalid_argument("USB data port is required");
    if (config.rs485_baud == 0u)
        return invalid_argument("RS485 baud rate must be positive");
    if (config.initial_attenuation_db > 127u)
        return invalid_argument("attenuation must be 0..127 dB");
    return voice_board_detail::open_device(impl_->context, config);
}

voice_board_result_t voice_board_t::close()
{
    return (!impl_ || !impl_->context)
        ? voice_board_result_t{}
        : voice_board_detail::close_device(impl_->context);
}

bool voice_board_t::is_open() const
{
    return impl_ && impl_->context &&
        voice_board_detail::device_is_open(impl_->context);
}

voice_board_result_t voice_board_t::load_script(
    uint8_t voice_id, std::string const& path)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (voice_id >= voice_count)
        return invalid_argument("voice must be 0..7");
    if (path.empty()) return invalid_argument("BEC file is required");
    return voice_board_detail::load_script(impl_->context, voice_id, path);
}

voice_board_result_t voice_board_t::load_sample(
    uint16_t sample_id, std::vector<int16_t> const& pcm,
        uint32_t source_sample_rate_hz, double root_pitch_hz)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (sample_id > 247u) return invalid_argument("sample ID must be 0..247");
    if (pcm.empty()) return invalid_argument("sample PCM is empty");
    if (source_sample_rate_hz != 48000u)
        return invalid_argument("sample rate must be 48000 Hz");
    if (!(root_pitch_hz > 0.0) || !std::isfinite(root_pitch_hz))
        return invalid_argument("root pitch must be positive and finite");
    return voice_board_detail::load_sample(
        impl_->context, sample_id, pcm, root_pitch_hz);
}

voice_board_result_t voice_board_t::note_on(
    uint8_t voice_id, uint16_t sample_id, uint8_t midi_key, uint8_t velocity)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (voice_id >= voice_count)
        return invalid_argument("voice must be 0..7");
    if (sample_id > 247u) return invalid_argument("sample ID must be 0..247");
    if (midi_key > 127u) return invalid_argument("MIDI key must be 0..127");
    if (velocity == 0u || velocity > 127u)
        return invalid_argument("velocity must be 1..127");
    return voice_board_detail::note_on(
        impl_->context, voice_id, sample_id, midi_key, velocity);
}

voice_board_result_t voice_board_t::note_off(uint8_t voice_id)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (voice_id >= voice_count)
        return invalid_argument("voice must be 0..7");
    return voice_board_detail::note_off(impl_->context, voice_id);
}

voice_board_result_t voice_board_t::all_notes_off()
{
    if (!impl_ || !impl_->context) return unavailable();
    return voice_board_detail::all_notes_off(impl_->context);
}

voice_board_result_t voice_board_t::set_attenuation(uint8_t attenuation_db)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (attenuation_db > 127u)
        return invalid_argument("attenuation must be 0..127 dB");
    return voice_board_detail::set_attenuation(impl_->context, attenuation_db);
}
