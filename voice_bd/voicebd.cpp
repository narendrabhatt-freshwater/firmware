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
 * RS485 carries note and status commands. CDC is used for BEC and ATTACK
 * uploads. The sample BODY is streamed over USB audio.
 */

#include <RtAudio.h>
#include <libserialport.h>
#if defined(__APPLE__)
// To change macOS serial latency from the default 16 ms to 1 ms.
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#endif
#if defined(__linux__)
#include <fstream>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>

namespace voice_board_detail {
namespace {

/* Channel Card wire-format constants. */

constexpr unsigned kVoiceCount = voice_board_t::voice_count;
constexpr unsigned kSampleSlotCount = 248u;
constexpr unsigned kSampleRate = 48000u;
constexpr unsigned kAttackSampleCount = 512u;
constexpr unsigned kCrossfadeSampleCount = 32u;
constexpr unsigned kUsbAudioChannelCount = 21u;
constexpr unsigned kUsbAudioFramesPerMillisecond = 48u;
constexpr unsigned kUsbPacketByteCount =
    kUsbAudioChannelCount * kUsbAudioFramesPerMillisecond;
constexpr unsigned kUsbPacketHeaderByteCount = 10u;
constexpr unsigned kUsbPacketBodySampleCount =
    kUsbPacketByteCount - kUsbPacketHeaderByteCount;
constexpr uint8_t kUsbPacketTag = 0xA0u;
constexpr uint8_t kUsbStartOfVoiceFlag = 0x08u;
constexpr uint8_t kUsbIdlePacketTag = 0xFFu;
constexpr uint8_t kSessionIdWrap = 255u;
constexpr size_t kBecHeaderBytes = 20u;
constexpr size_t kBecMaxPayload = 16384u;
/* Enough history to cover packets sent between status replies. */
constexpr size_t kPendingPacketCapacity = 256u;
constexpr std::array<uint8_t, 4> kBecMagic{{0x46u, 0x57u, 0x53u, 0x43u}};

/*******************************************************************************

                     v o i c e   b o a r d   h e l p e r s

*******************************************************************************/

/* ---- return a successful board result ------------------------------------ */

voice_board_result_t ok(std::string message = {})
{
    return {voice_board_error_t::ok, std::move(message)};
}

/* ---- return a failed board result ---------------------------------------- */

voice_board_result_t fail(voice_board_error_t code, std::string message)
{
    return {code, std::move(message)};
}

/* ---- match an audio stream device name ----------------------------------- */

bool stream_name_matches(std::string const& device_name,
    std::string const& requested_name)
{
    if (device_name == requested_name) return true;
    if (device_name.size() <= requested_name.size()) return false;
    size_t const name_start = device_name.size() - requested_name.size();
    return device_name.compare(name_start, requested_name.size(),
        requested_name) == 0 &&
        device_name[name_start - 1u] == ' ';
}

/* ---- read a little-endian 16 bit value ----------------------------------- */

uint16_t read16(uint8_t const* p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

/* ---- read a little-endian 32 bit value ----------------------------------- */

uint32_t read32(uint8_t const* p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8u) |
        (static_cast<uint32_t>(p[2]) << 16u) |
        (static_cast<uint32_t>(p[3]) << 24u);
}

/* ---- calculate the payload crc32 ----------------------------------------- */

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

/*******************************************************************************

                             s e r i a l   p o r t

*******************************************************************************/

/* ---- format a serial port error ------------------------------------------ */

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

class serial_port
{
private:
    struct sp_port* port_ = nullptr;
    bool opened_ = false;
    enum sp_return result_ = SP_OK;

public:
    serial_port() = default;
/* ---- close the serial port ----------------------------------------------- */

    ~serial_port() { close(); }
    serial_port(serial_port const&) = delete;
    serial_port& operator=(serial_port const&) = delete;

/* ---- open the serial port ------------------------------------------------ */

    bool open(std::string const& name, uint32_t baud, bool assert_dtr,
        std::string& error)
    {
        close();
        enum sp_return result = sp_get_port_by_name(name.c_str(), &port_);
        if (result != SP_OK) {
            error = serial_error("locating " + name, result);
            return false;
        }
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
#if defined(__linux__)
	if (!assert_dtr) {
		std::string const tty = name.substr(name.find_last_of('/') + 1);
		std::string const latency_path = "/sys/bus/usb-serial/devices/" +
						tty +
						"/latency_timer";
		{
			std::ofstream out(latency_path);
			if (!out) {
				error = "cannot set low-latency mode on " + name;
				close();
				return false;
			}
			out << "1/n";
		}
		{
			std::ifstream in(latency_path);
			unsigned latency = 0;
			if (!(in >> latency) || latency != 1u) {
				error = "failed to enable 1 ms latency on " + name;
				close();
				return false;
			}
		}
	}
#endif
        if (assert_dtr) (void)sp_set_dtr(port_, SP_DTR_ON);
        (void)sp_flush(port_, SP_BUF_BOTH);
        return true;
    }

/* ---- close the serial port ----------------------------------------------- */

    void close()
    {
        if (!port_) return;
        if (opened_) (void)sp_close(port_);
        sp_free_port(port_);
        port_ = nullptr;
        opened_ = false;
    }

/* ---- write to the serial port -------------------------------------------- */

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
        result_ = sp_drain(port_);
        if (result_ != SP_OK) {
            error = serial_error("serial drain", result_);
            return false;
        }
        return true;
    }

/* ---- read from the serial port ------------------------------------------- */

    size_t read(void* data, size_t capacity, unsigned timeout_ms)
    {
        if (!opened_ || capacity == 0u) return 0u;
        enum sp_return const result = sp_blocking_read_next(
            port_, data, capacity, timeout_ms == 0u ? 1u : timeout_ms);
        return result > 0 ? static_cast<size_t>(result) : 0u;
    }

/* ---- flush serial input -------------------------------------------------- */

    void flush() { if (opened_) (void)sp_flush(port_, SP_BUF_INPUT); }
/* ---- test whether the serial port is open -------------------------------- */

    bool is_open() const { return opened_; }

};

/* ---- wait for a serial response ------------------------------------------ */

bool wait_for(serial_port& port, char const* wanted, std::string& received,
    unsigned timeout_ms)
{
    auto const stop_waiting_at = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    std::array<uint8_t, 256> bytes{};
    while (std::chrono::steady_clock::now() < stop_waiting_at) {
        size_t const count = port.read(bytes.data(), bytes.size(), 20u);
        received.append(reinterpret_cast<char const*>(bytes.data()), count);
        if (received.find(wanted) != std::string::npos) return true;
        if (received.find("err:") != std::string::npos) return false;
    }
    return false;
}

/* ---- send an upload payload ---------------------------------------------- */

voice_board_result_t send_payload(serial_port& port, std::string const& command,
    uint8_t const* data, size_t size,
    char const* completion)
{
    port.flush();
    std::string error;
    std::string const line = "c:" + command + " " + std::to_string(size) + "\r";
    if (!port.write(line.data(), line.size(), error))
        return fail(voice_board_error_t::io_error, error);
    std::string reply;
    if (!wait_for(port, "ok:ready", reply, 2000u))
        return fail(voice_board_error_t::bad_reply,
            reply.empty() ? "card did not become ready for upload" : reply);
    reply.clear();
    for (size_t offset = 0; offset < size;) {
        size_t const count = std::min<size_t>(512u, size - offset);
        if (!port.write(data + offset, count, error))
            return fail(voice_board_error_t::io_error, error);
        offset += count;
    }
    if (!wait_for(port, completion, reply, 3000u))
        return fail(voice_board_error_t::bad_reply,
            reply.empty() ? "card did not confirm upload" : reply);
    return ok();
}

/*******************************************************************************

                     b e c   p r o g r a m   l o a d i n g

*******************************************************************************/

/* Check the BEC header and CRC before touching the card. */

/* ---- read and validate the channel program ------------------------------- */

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

/*******************************************************************************

                              r s 4 8 5   l i n k

*******************************************************************************/

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

/* ---- parse voice queue status -------------------------------------------- */

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

class rs485_link
{
private:
    serial_port port_;
    std::mutex mutex_;

public:
/* ---- open the rs485 link ------------------------------------------------- */

    voice_board_result_t open(std::string const& path, uint32_t baud)
    {
        std::string error;
        if (!port_.open(path, baud, false, error))
            return fail(voice_board_error_t::io_error, error);
        return ok();
    }

/* ---- close the serial port ----------------------------------------------- */

    void close() { port_.close(); }
/* ---- test whether the serial port is open -------------------------------- */

    bool is_open() const { return port_.is_open(); }

/* ---- send an rs485 command ----------------------------------------------- */

    voice_board_result_t command(std::string const& body,
        std::vector<uint8_t>* binary = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string const wire = (body.size() > 1u && body[1] == ':')
            ? body + "\r" : "c:" + body + "\r";
        port_.flush();
        std::string error;
        if (!port_.write(wire.data(), wire.size(), error))
            return fail(voice_board_error_t::io_error, error);
        std::vector<uint8_t> reply;
        auto const stop_waiting_at = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5);
        std::array<uint8_t, 256> bytes{};
        while (std::chrono::steady_clock::now() < stop_waiting_at) {
            size_t const count = port_.read(
                bytes.data(), bytes.size(), 1u);
            // std::cerr << "bytes " << bytes.size() << std::endl;
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

/*******************************************************************************

                        u s b   a u d i o   s t r e a m

*******************************************************************************/

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
    bool waiting_for_first_packet = false;
    unsigned initial_samples_left = 0u;
    bool filling_initial_buffer = false;
    bool note_seen_in_status = false;
    uint8_t session_id = 0u;
    uint16_t sample_id = 0u;
    size_t source_position = 0u;
    size_t stream_origin = 0u;
    unsigned available_sample_slots = 0u;
    bool pending = true;
    unsigned refill_samples_5ms = 0u;
    double refill_balance = 0.0;
    std::chrono::steady_clock::time_point refill_at{};
    std::chrono::steady_clock::time_point deadline{};
};

/* A USB packet that the card has not acknowledged yet. */
struct pending_packet
{
    uint16_t sequence = 0u;
    uint8_t voice = 0u;
    uint8_t session = 0u;
    uint16_t samples = 0u;
    std::chrono::steady_clock::time_point packet_at{};
};

/* State shared by the RtAudio callback and the status thread. */

struct stream_state
{
    std::mutex mutex;
    std::array<sample_data, kSampleSlotCount> samples;
    std::array<voice_data, kVoiceCount> voices;

    /* Sent packets stay here until the card reports their sequence numbers. */
    std::array<pending_packet, kPendingPacketCapacity> pending_packets{};
    size_t first_pending_packet = 0u;
    size_t pending_packet_count = 0u;
    uint16_t next_sequence = 0u;
    bool sequence_ready = false;
    std::array<int8_t, kUsbPacketByteCount> packet{};
    size_t packet_offset = kUsbPacketByteCount;
    uint8_t next_voice = 0u;
    uint8_t last_status_sequence = 0u;
    std::chrono::steady_clock::time_point acknowledged_packet_at{};
    bool status_ready = false;
    unsigned buffer_capacity = 0u;
    std::chrono::steady_clock::time_point next_packet_at{};


/* ---- apply voice queue status -------------------------------------------- */

    void apply_status(card_status const& status,
        std::chrono::steady_clock::time_point requested_at)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (status_ready) {
            uint8_t const distance = static_cast<uint8_t>(
                status.status_sequence - last_status_sequence);
            if (distance == 0u || distance >= 0x80u) return;
        }
        last_status_sequence = status.status_sequence;
        status_ready = true;
        buffer_capacity = status.buffer_capacity;
        if (!sequence_ready) {
            next_sequence = static_cast<uint16_t>(
                status.last_usb_sequence + 1u);
            sequence_ready = true;
        }
        /* Drop packets already consumed by the card. */
        while (pending_packet_count != 0u) {
            auto const& front = pending_packets[first_pending_packet];
            uint16_t const distance = static_cast<uint16_t>(
                status.last_usb_sequence - front.sequence);
            if (distance >= 0x8000u) break;
            acknowledged_packet_at = front.packet_at;
            first_pending_packet =
                (first_pending_packet + 1u) % kPendingPacketCapacity;
            --pending_packet_count;
        }
        std::array<unsigned, kVoiceCount> samples_in_flight{};
        for (size_t i = 0; i < pending_packet_count; ++i) {
            auto const& packet = pending_packets[
                (first_pending_packet + i) % kPendingPacketCapacity];
            samples_in_flight[packet.voice] += packet.samples;
        }
        uint8_t const live_voice_mask = static_cast<uint8_t>(
            status.active_voice_mask | status.pending_voice_mask);
        for (uint8_t voice = 0; voice < kVoiceCount; ++voice) {
            auto& state = voices[voice];
            unsigned const free_slots =
                status.free_samples[voice] > samples_in_flight[voice]
                    ? status.free_samples[voice] - samples_in_flight[voice]
                    : 0u;
            if (!state.active) {
                state.available_sample_slots = free_slots;
                continue;
            }
            if ((live_voice_mask & static_cast<uint8_t>(1u << voice)) == 0u ||
                    status.session_id[voice] != state.session_id) {
                state.available_sample_slots = 0u;
                state.refill_samples_5ms = 0u;
                if (state.note_seen_in_status &&
                    (live_voice_mask & (1u << voice)) == 0u)
                    state.active = false;
                continue;
            }
            state.note_seen_in_status = true;
            state.available_sample_slots = free_slots;
            state.pending = (status.pending_voice_mask & (1u << voice)) != 0u;
            state.deadline = requested_at +
                std::chrono::microseconds(status.remaining_us[voice]);
            state.waiting_for_first_packet = state.pending;
            state.refill_samples_5ms = !state.pending
                ? status.refill_samples_5ms[voice] : 0u;
            /* Map the status snapshot to the acknowledged USB packet. */
            state.refill_at = acknowledged_packet_at != std::chrono::steady_clock::time_point{}
                    && status.usb_age_ms != 255u
                ? acknowledged_packet_at + std::chrono::milliseconds(status.usb_age_ms)
                : std::max(requested_at, next_packet_at);
            /* Account for outstanding samples in USB packet order. */
            state.refill_balance = status.free_samples[voice];
            for (size_t j = 0; j < pending_packet_count; ++j) {
                auto const& sent = pending_packets[
                    (first_pending_packet + j) % kPendingPacketCapacity];
                if (sent.voice != voice) continue;
                if (state.refill_samples_5ms && sent.packet_at > state.refill_at) {
                    double elapsed_ms = std::chrono::duration<double, std::milli>(
                        sent.packet_at - state.refill_at).count();
                    state.refill_balance = std::min(double(buffer_capacity),
                        state.refill_balance + elapsed_ms * state.refill_samples_5ms / 5.0);
                    state.refill_at = sent.packet_at;
                }
                state.refill_balance -= sent.samples;
            }
        }

    }

/* ---- update startup priority --------------------------------------------- */

    void update_startup_priority(voice_data& voice)
    {
        if (!voice.active || !voice.filling_initial_buffer) return;
        double const reserve = std::ceil(2.0 * voice.refill_samples_5ms / 5.0);
        double const target = voice.pending || !voice.note_seen_in_status
            ? double(buffer_capacity)
            : std::min(double(buffer_capacity) - reserve,
                double(std::max(kUsbPacketBodySampleCount, voice.refill_samples_5ms)));
        double const buffered = voice.pending || !voice.note_seen_in_status
            ? double(voice.source_position - voice.stream_origin)
            : voice.refill_samples_5ms
                ? buffer_capacity - voice.refill_balance
                : buffer_capacity - voice.available_sample_slots;
        if (voice.note_seen_in_status && !voice.pending &&
            std::ceil(buffered) >= target) {
            voice.filling_initial_buffer = false;
            voice.initial_samples_left = 0u;
        } else {
            voice.initial_samples_left = static_cast<unsigned>(
                std::max(1.0, std::ceil(target - buffered)));
        }
    }

/* ---- prepare the next usb audio packet ----------------------------------- */

    void make_packet(std::chrono::steady_clock::time_point now =
                         std::chrono::steady_clock::now())
    {
        std::fill(packet.begin(), packet.end(), 0);
        packet[2] = packet[6] = static_cast<int8_t>(kUsbIdlePacketTag);
        packet_offset = 0u;
        /* Advance the USB packet time. */
        auto const packet_at = next_packet_at == std::chrono::steady_clock::time_point{}
            ? now : next_packet_at;
        next_packet_at = packet_at + std::chrono::milliseconds(1);
        if (!sequence_ready || pending_packet_count + 2u > kPendingPacketCapacity)
            return;
        for (auto& voice : voices) {
            if (!voice.active || voice.refill_samples_5ms == 0u) continue;
            double const elapsed_ms = std::chrono::duration<double, std::milli>(
                packet_at - voice.refill_at).count();
            if (elapsed_ms > 0.0) {
                voice.refill_balance = std::min(double(buffer_capacity),
                    voice.refill_balance + elapsed_ms * voice.refill_samples_5ms / 5.0);
                voice.refill_at = packet_at;
            }
            /* Calculate the refill allowance. */
            double const reserve = std::ceil(2.0 * voice.refill_samples_5ms / 5.0);
            double const allowance = voice.refill_balance - reserve;
            voice.available_sample_slots = allowance > 0.0
                ? static_cast<unsigned>(allowance) : 0u;
        }
        for (auto& voice : voices) update_startup_priority(voice);

        std::array<unsigned, kVoiceCount> queued_samples{};
        for (size_t i = 0u; i < pending_packet_count; ++i) {
            auto const& queued = pending_packets[
                (first_pending_packet + i) % kPendingPacketCapacity];
            if (queued.session == voices[queued.voice].session_id)
                queued_samples[queued.voice] += queued.samples;
        }
        std::array<uint8_t, kVoiceCount> candidates{};
        size_t count = 0u;
        auto urgent = [&](uint8_t v) {
            return voices[v].initial_samples_left != 0u || voices[v].pending ||
                voices[v].deadline <= now + std::chrono::milliseconds(10);
        };
        for (uint8_t i = 0u; i < kVoiceCount; ++i) {
            uint8_t v = static_cast<uint8_t>((next_voice + i) % kVoiceCount);
            if (voices[v].active &&
                voices[v].available_sample_slots != 0u &&
                samples[voices[v].sample_id].pcm_size != 0u) candidates[count++] = v;
        }
        auto protect = [&](uint8_t v) -> unsigned {
            auto const& voice = voices[v];
            if (voice.pending || voice.initial_samples_left) return 0u;
            if (!voice.refill_samples_5ms)
                return voice.deadline <= now + std::chrono::milliseconds(2)
                    ? voice.available_sample_slots : 0u;
            /* Calculate the playing voice's refill requirement. */
            double const needed = std::ceil(3.0 * voice.refill_samples_5ms / 5.0) -
                (buffer_capacity - voice.refill_balance);
            return needed > 0.0 ? std::min(voice.available_sample_slots,
                static_cast<unsigned>(std::ceil(needed))) : 0u;
        };
        bool const has_startup = std::any_of(
            candidates.begin(), candidates.begin() + count,
            [&](uint8_t v) { return voices[v].filling_initial_buffer; });
        auto first_packet = [&](uint8_t v) {
            return voices[v].filling_initial_buffer &&
                voices[v].source_position - voices[v].stream_origin < kUsbPacketBodySampleCount;
        };
        auto before = [&](uint8_t a, uint8_t b) {
            if (first_packet(a) != first_packet(b)) return first_packet(a);
            if (has_startup && (protect(a) != 0u) != (protect(b) != 0u))
                return protect(a) != 0u;
            if ((voices[a].initial_samples_left != 0u) !=
                (voices[b].initial_samples_left != 0u))
                return voices[a].initial_samples_left != 0u;
            if (urgent(a) != urgent(b)) return urgent(a);
            if (voices[a].refill_samples_5ms && voices[b].refill_samples_5ms) {
                double const coverage_a = (buffer_capacity - voices[a].refill_balance)
                    / voices[a].refill_samples_5ms;
                double const coverage_b = (buffer_capacity - voices[b].refill_balance)
                    / voices[b].refill_samples_5ms;
                if (coverage_a != coverage_b) return coverage_a < coverage_b;
            }
            if (urgent(a) && queued_samples[a] != queued_samples[b])
                return queued_samples[a] < queued_samples[b];
            if (urgent(a) && voices[a].pending != voices[b].pending)
                return !voices[a].pending;
            if (voices[a].pending && voices[b].pending) return false;
            return voices[a].deadline < voices[b].deadline;
        };
        /* Eight entries: insertion sort is bounded and never allocates. */
        for (size_t i = 1u; i < count; ++i) {
            auto const candidate = candidates[i];
            size_t j = i;
            while (j != 0u && before(candidate, candidates[j - 1u])) {
                candidates[j] = candidates[j - 1u];
                --j;
            }
            candidates[j] = candidate;
        }
        if (count == 0u) return;
        bool const initial = voices[candidates[0]].initial_samples_left != 0u;
        size_t const blocks = count >= 2u &&
            (!first_packet(candidates[0]) ||
             voices[candidates[0]].available_sample_slots < kUsbPacketBodySampleCount) &&
            ((voices[candidates[0]].filling_initial_buffer || voices[candidates[1]].filling_initial_buffer) ||
             (urgent(candidates[0]) && urgent(candidates[1]) && (!initial ||
                voices[candidates[0]].available_sample_slots < kUsbPacketBodySampleCount)))
            ? 2u : 1u;

        std::array<unsigned, 2> sizes{};
        unsigned const budget = kUsbPacketBodySampleCount;
        sizes[0] = std::min(voices[candidates[0]].available_sample_slots,
                           blocks == 2u && !initial ? budget / 2u : budget);
        if (blocks == 2u) {
            auto const& a = voices[candidates[0]];
            auto const& b = voices[candidates[1]];
            bool const starting_a = a.initial_samples_left != 0u;
            bool const starting_b = b.initial_samples_left != 0u;
            if ((a.filling_initial_buffer || b.filling_initial_buffer) && starting_a != starting_b) {
                if (starting_a) {
                    sizes[1] = std::min(protect(candidates[1]), budget);
                    sizes[0] = std::min(a.available_sample_slots, budget - sizes[1]);
                    sizes[1] = std::min(b.available_sample_slots, budget - sizes[0]);
                } else {
                    sizes[0] = std::min(protect(candidates[0]), budget);
                    sizes[1] = std::min(b.available_sample_slots, budget - sizes[0]);
                    sizes[0] = std::min(a.available_sample_slots, budget - sizes[1]);
                }
            } else {
                if (!initial && a.refill_samples_5ms && b.refill_samples_5ms) {
                    /* Divide the payload using the reported sample demand. */
                    double const wanted = (a.refill_samples_5ms *
                        (buffer_capacity - b.refill_balance + budget) -
                        b.refill_samples_5ms * (buffer_capacity - a.refill_balance)) /
                        (a.refill_samples_5ms + b.refill_samples_5ms);
                    sizes[0] = std::min(a.available_sample_slots,
                        static_cast<unsigned>(std::clamp(wanted, 0.0, double(budget))));
                }
                sizes[1] = std::min(voices[candidates[1]].available_sample_slots,
                                   budget - sizes[0]);
                sizes[0] = std::min(voices[candidates[0]].available_sample_slots,
                                   budget - sizes[1]);
            }
        }

        packet[0] = static_cast<int8_t>(next_sequence & 0xFFu);
        packet[1] = static_cast<int8_t>(next_sequence >> 8u);
        size_t offset = kUsbPacketHeaderByteCount;
        for (size_t i = 0u; i < blocks; ++i) {
            uint8_t const chosen = candidates[i];
            auto& voice = voices[chosen];
            auto& sample = samples[voice.sample_id];
            size_t const at = 2u + 4u * i;
            packet[at] = static_cast<int8_t>(kUsbPacketTag |
                (voice.waiting_for_first_packet ? kUsbStartOfVoiceFlag : 0u) | chosen);
            packet[at + 1u] = static_cast<int8_t>(voice.session_id);
            packet[at + 2u] = static_cast<int8_t>(sizes[i] & 0xFFu);
            packet[at + 3u] = static_cast<int8_t>(sizes[i] >> 8u);
            for (unsigned j = 0u; j < sizes[i] &&
                    voice.source_position < sample.pcm_size; ++j)
                packet[offset + j] = static_cast<int8_t>(sample.pcm[voice.source_position++] >> 8);
            offset += sizes[i];
            pending_packets[(first_pending_packet + pending_packet_count) %
                kPendingPacketCapacity] = {next_sequence, chosen, voice.session_id,
                    static_cast<uint16_t>(sizes[i]), packet_at};
            ++pending_packet_count;
            voice.available_sample_slots -= sizes[i];
            if (voice.refill_samples_5ms != 0u) voice.refill_balance -= sizes[i];
            voice.initial_samples_left -= std::min(voice.initial_samples_left, sizes[i]);
            update_startup_priority(voice);
        }
        next_voice = static_cast<uint8_t>((candidates[blocks - 1u] + 1u) % kVoiceCount);
        ++next_sequence;
    }

/* ---- render the usb audio stream ----------------------------------------- */

    void render(int8_t* output, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t written = 0u;
        while (written < size) {
            if (packet_offset == kUsbPacketByteCount) make_packet();
            size_t const count = std::min(size - written,
                kUsbPacketByteCount - packet_offset);
            std::copy_n(packet.data() + packet_offset, count, output + written);
            packet_offset += count;
            written += count;
        }
    }
};

} // namespace

/*******************************************************************************

                          d e v i c e   c o n t e x t

*******************************************************************************/

struct device_context
{
    voice_board_config_t config;
    std::string rs485_name;
    std::string upload_usb_port_name;
    rs485_link rs485;
    serial_port upload_port;
    stream_state stream;
#if defined(__linux__)
    RtAudio audio{RtAudio::LINUX_ALSA};
#else
    RtAudio audio;
#endif
    std::atomic<bool> connected{false};
    std::atomic<bool> worker_running{false};
    std::thread worker;
    std::mutex upload_mutex;
    std::mutex control_mutex;
    std::condition_variable control_changed;
    std::atomic<unsigned> waiting_commands{0u};
    std::chrono::steady_clock::time_point next_poll = std::chrono::steady_clock::now();

    // MIDI commands take the next turn after any in-progress status query.
    struct control_turn {
        device_context* state;
        std::unique_lock<std::mutex> lock;
        explicit control_turn(device_context* s)
            : state(s) {
            ++state->waiting_commands;
            state->control_changed.notify_all();
            lock = std::unique_lock<std::mutex>(state->control_mutex);
            if (state->worker_running.load() &&
                std::chrono::steady_clock::now() >= state->next_poll) {
                (void)state->query_status();
                state->advance_poll();
            }
        }
        ~control_turn() {
            --state->waiting_commands;
            state->control_changed.notify_all();
        }
    };

/* ---- shut down the channel device context -------------------------------- */

    ~device_context() { shutdown(); }

/* ---- start the usb audio stream ------------------------------------------ */

    voice_board_result_t start_audio()
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
    try
#endif
    {
        if (audio.isStreamRunning()) return ok();
        unsigned selected = 0u;
        bool found = false;
        std::string available_devices;
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
        auto const device_ids = audio.getDeviceIds();
#else
        std::vector<unsigned> device_ids(audio.getDeviceCount());
        for (unsigned id = 0; id < device_ids.size(); ++id) device_ids[id] = id;
#endif
        for (unsigned id : device_ids) {
            RtAudio::DeviceInfo const info = audio.getDeviceInfo(id);
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
            if (!info.probed) continue;
#endif
            if (info.outputChannels < kUsbAudioChannelCount) continue;
            if (!available_devices.empty()) available_devices += ", ";
            available_devices += info.name;
            bool const matches =
                stream_name_matches(info.name, config.stream_usb_port);
            if (matches) {
                if (found)
                    return fail(voice_board_error_t::device_ambiguous,
                    "multiple stream devices named " +
                    config.stream_usb_port);
                selected = id;
                found = true;
            }
        }
        if (!found) {
            if (available_devices.empty()) available_devices = "none";
            return fail(voice_board_error_t::device_not_found,
                "stream device not found: " + config.stream_usb_port +
                    "; available 21-channel outputs: " + available_devices);
        }
        RtAudio::StreamParameters output;
        output.deviceId = selected;
        output.nChannels = kUsbAudioChannelCount;
        output.firstChannel = 0u;
        unsigned frames = kUsbAudioFramesPerMillisecond;
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
        RtAudioErrorType const opened =
#endif
        audio.openStream(
            &output, nullptr, RTAUDIO_SINT8, kSampleRate, &frames,
                [](void* out, void*, unsigned count, double,
                    RtAudioStreamStatus,
            void* user) -> int {
                static_cast<device_context *>(user)->stream.render(
                    static_cast<int8_t*>(out), count * kUsbAudioChannelCount);
                    return 0;
                }, this);
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
        if (opened != RTAUDIO_NO_ERROR)
            return fail(voice_board_error_t::audio_error, audio.getErrorText());
        RtAudioErrorType const started = audio.startStream();
        if (started != RTAUDIO_NO_ERROR) {
            std::string const error = audio.getErrorText();
            audio.closeStream();
            return fail(voice_board_error_t::audio_error, error);
        }
#else
        audio.startStream();
#endif
        return ok();
    }
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
    catch (RtAudioError const& error) {
        if (audio.isStreamOpen()) audio.closeStream();
        return fail(voice_board_error_t::audio_error, error.getMessage());
    }
#endif

/* ---- shut down the channel device ---------------------------------------- */

    void shutdown()
    {
        std::lock_guard<std::mutex> upload_lock(upload_mutex);
        worker_running.store(false);
        control_changed.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
            worker.join();
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
        if (audio.isStreamRunning()) (void)audio.stopStream();
#else
        try {
            if (audio.isStreamRunning()) audio.stopStream();
        } catch (RtAudioError const&) {
            /* Continue closing resources even if stopping the driver failed. */
        }
#endif
        if (audio.isStreamOpen()) audio.closeStream();
        upload_port.close();
        if (rs485.is_open()) {
            if (connected.load()) (void)rs485.command("n off");
            rs485.close();
        }
        {
            std::lock_guard<std::mutex> lock(stream.mutex);
            for (auto& voice : stream.voices) voice = {};
            stream.first_pending_packet = 0u;
            stream.pending_packet_count = 0u;
            stream.sequence_ready = false;
            stream.status_ready = false;
            stream.next_packet_at = {};
            stream.acknowledged_packet_at = {};
            stream.packet_offset = kUsbPacketByteCount;
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
        replacement.pcm.reset(); // Free old PCM outside the audio lock.
        auto result = ok();
        for (uint8_t i = 0; i < kVoiceCount; ++i) {
            std::unique_lock<std::mutex> lock(stream.mutex);
            auto& voice = stream.voices[i];
            if (!voice.active || voice.sample_id != id ||
                    voice.source_position < stream.samples[id].pcm_size) continue;
            voice.active = false;
            lock.unlock();
            auto off = rs485.command("n" + std::to_string(i) + " off");
            if (!off && result) {
                result = off;
                result.message = "sample committed; replacement note-off failed: " + off.message;
            }
        }
        return result;
    }

/* ---- poll channel card status -------------------------------------------- */

    voice_board_result_t query_status()
    {
        auto const requested_at = std::chrono::steady_clock::now();
        std::vector<uint8_t> raw;
        auto result = rs485.command("vq", &raw);
        if (result) {
            card_status status;
            if (parse_voice_queue_status(raw, status))
                stream.apply_status(status, requested_at);
            else result = fail(voice_board_error_t::bad_reply, "invalid voice status reply");
        }
        return result;
    }

    void advance_poll()
    {
        auto const now = std::chrono::steady_clock::now();
        next_poll += std::chrono::milliseconds(5);
        if (next_poll < now) next_poll = now;
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
            if (std::chrono::steady_clock::now() < next_poll) {
                control_changed.wait_until(lock, next_poll);
                continue;
            }
            (void)query_status();
            advance_poll();
        }
    }

};

/* ---- create a channel device context ------------------------------------- */

device_context* create() { return new (std::nothrow) device_context; }

/* ---- destroy a channel device context ------------------------------------ */

void destroy(device_context* state)
{
    delete state;
}

/* ---- upload a BEC program to one voice ----------------------------------- */

voice_board_result_t upload_script(device_context* state, uint8_t voice_id,
    std::vector<uint8_t> const& bec)
{
    /* vmload rejects active voices with err:vm-busy. */
    auto result = send_payload(state->upload_port,
        "vmload " + std::to_string(voice_id),
        bec.data(), bec.size(), "ok:vm");
    if (!result) {
        result.code = voice_board_error_t::bec_error;
        result.message = "voice " + std::to_string(voice_id) +
            " BEC upload failed: " + result.message;
    }
    return result;
}

/* ---- open the channel device --------------------------------------------- */

voice_board_result_t open_device(device_context* state,
    voice_board_config_t const& config)
{
    if (!state) return fail(voice_board_error_t::io_error,
        "voice board state is unavailable");
    if (state->connected.load()) return ok("already connected");
    /* Read the program first so a bad file cannot disturb the card. */
    std::vector<uint8_t> bec;
    auto result = read_bec(config.bec_file, bec);
    if (!result) return result;
    state->config = config;
    state->rs485_name = config.rs485_port;
    state->upload_usb_port_name = config.upload_usb_port;
    result = state->rs485.open(state->rs485_name, config.rs485_baud);
    if (!result) return result;
    /* Send a clear command to clear the card-side input line. */
    result = state->rs485.command("clear");
    result = state->query_status();
    if (!result) {
        state->shutdown();
        return result;
    }
    std::string error;
    if (!state->upload_port.open(state->upload_usb_port_name, 115200u, true, error)) {
        state->shutdown();
        return fail(voice_board_error_t::io_error, error);
    }
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
    result = state->start_audio();
    if (!result) {
        state->shutdown();
        return result;
    }
    state->connected.store(true);
    state->next_poll = std::chrono::steady_clock::now();
    state->worker_running.store(true);
    state->worker = std::thread([state] { state->poll(); });
    return ok("Channel Card connected; BEC loaded into voices 0-7");
}

/* ---- close the channel device -------------------------------------------- */

voice_board_result_t close_device(device_context* state)
{
    if (state) state->shutdown();
    return ok("voice board disconnected");
}

/* ---- test whether the channel device is open ----------------------------- */

bool device_is_open(device_context const* state)
{
    return state && state->connected.load();
}

/* ---- load a BEC program into the channel device -------------------------- */

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

/* ---- load a sample into the channel device ------------------------------- */

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

    // CDC has its own serialization: a slow upload must not hold up vq or MIDI.
    std::lock_guard<std::mutex> upload_lock(state->upload_mutex);
    auto result = send_payload(state->upload_port, "al " + std::to_string(sample_id),
        reinterpret_cast<uint8_t const*>(attack.data()), attack_size, "ok:attack");
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

/* ---- start a channel voice ----------------------------------------------- */

voice_board_result_t note_on(device_context* state, uint8_t voice,
    uint16_t sample,
    uint8_t key, uint8_t velocity)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    auto result = state->start_audio();
    if (!result) return result;
    device_context::control_turn turn(state);
    uint8_t session = 0u;
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        if (!state->stream.samples[sample].loaded)
            return fail(voice_board_error_t::sample_error,
                "sample " + std::to_string(sample) + " is not loaded");
        auto& slot = state->stream.voices[voice];
        session = static_cast<uint8_t>((slot.session_id + 1u) % kSessionIdWrap);
        unsigned const free_slots = slot.available_sample_slots;
        slot = {};
        slot.available_sample_slots = free_slots;
        slot.waiting_for_first_packet = true;
        slot.initial_samples_left = kUsbPacketBodySampleCount;
        slot.filling_initial_buffer = true;
        slot.session_id = session;
        slot.sample_id = sample;
        size_t const attack_size = std::min<size_t>(
            kAttackSampleCount, state->stream.samples[sample].pcm_size);
        slot.source_position = attack_size -
            std::min<size_t>(kCrossfadeSampleCount, attack_size);
        slot.stream_origin = slot.source_position;
    }
    std::string const note = "n" + std::to_string(voice) + " on " +
        std::to_string(sample) + " " + std::to_string(key) + " " +
        std::to_string(velocity) + " @" + std::to_string(session);
    result = state->rs485.command(note);
    if (!result) {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        state->stream.voices[voice].active = false;
        state->stream.voices[voice].available_sample_slots = 0u;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        state->stream.voices[voice].active = true;
    }
    return ok("voice " + std::to_string(voice) + " started");
}

/* ---- release a channel voice --------------------------------------------- */

voice_board_result_t note_off(device_context* state, uint8_t voice)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    device_context::control_turn turn(state);
    auto const result = state->rs485.command(
        "n" + std::to_string(voice) + " off");
    return result;
}

/* ---- release all channel voices ------------------------------------------ */

voice_board_result_t all_notes_off(device_context* state)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    device_context::control_turn turn(state);
    auto const result = state->rs485.command("n off");
    return result;
}

/* ---- set channel output attenuation -------------------------------------- */

voice_board_result_t set_attenuation(device_context* state,
    uint8_t attenuation)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    return state->rs485.command("g 1 " + std::to_string(attenuation));
}

} // namespace voice_board_detail

/*******************************************************************************

                   v o i c e   b o a r d   i n t e r f a c e

*******************************************************************************/

struct voice_board_t::impl_t
{
    voice_board_detail::device_context* context = voice_board_detail::create();
/* ---- destroy the board implementation ------------------------------------ */

    ~impl_t() { voice_board_detail::destroy(context); }
};

namespace {

/* ---- return an invalid argument result ----------------------------------- */

voice_board_result_t invalid_argument(std::string const& message)
{
    return {voice_board_error_t::invalid_argument, message};
}

/* ---- return an unavailable board result ---------------------------------- */

voice_board_result_t unavailable()
{
    return {voice_board_error_t::io_error,
        "voice board implementation is unavailable"};
}

} // namespace

/* ---- construct the voice board ------------------------------------------- */

voice_board_t::voice_board_t() : impl_(std::make_unique<impl_t>()) {}
/* ---- destroy the voice board --------------------------------------------- */

voice_board_t::~voice_board_t() = default;
/* ---- move construct the voice board -------------------------------------- */

voice_board_t::voice_board_t(voice_board_t&&) noexcept = default;
/* ---- move assign the voice board ----------------------------------------- */

voice_board_t& voice_board_t::operator=(voice_board_t&&) noexcept = default;

/* ---- open the voice board ------------------------------------------------ */

voice_board_result_t voice_board_t::open(voice_board_config_t const& config)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (config.bec_file.empty())
        return invalid_argument("BEC file is required");
    if (config.rs485_port.empty())
        return invalid_argument("RS485 port name is required");
    if (config.upload_usb_port.empty())
        return invalid_argument("upload USB port is required");
    if (config.stream_usb_port.empty())
        return invalid_argument("stream USB port is required");
    if (config.rs485_baud == 0u)
        return invalid_argument("RS485 baud rate must be positive");
    if (config.initial_attenuation_db > 127u)
        return invalid_argument("attenuation must be 0..127 dB");
    return voice_board_detail::open_device(impl_->context, config);
}

/* ---- close the voice board ----------------------------------------------- */

voice_board_result_t voice_board_t::close()
{
    return (!impl_ || !impl_->context)
        ? voice_board_result_t{}
        : voice_board_detail::close_device(impl_->context);
}

/* ---- test whether the voice board is open -------------------------------- */

bool voice_board_t::is_open() const
{
    return impl_ && impl_->context &&
        voice_board_detail::device_is_open(impl_->context);
}

/* ---- load a BEC program into one voice ----------------------------------- */

voice_board_result_t voice_board_t::load_script(
    uint8_t voice_id, std::string const& path)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (voice_id >= voice_count)
        return invalid_argument("voice must be 0..7");
    if (path.empty()) return invalid_argument("BEC file is required");
    return voice_board_detail::load_script(impl_->context, voice_id, path);
}

/* ---- load a sample into the voice board ---------------------------------- */

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

/* ---- start a voice board note -------------------------------------------- */

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

/* ---- release a voice board note ------------------------------------------ */

voice_board_result_t voice_board_t::note_off(uint8_t voice_id)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (voice_id >= voice_count)
        return invalid_argument("voice must be 0..7");
    return voice_board_detail::note_off(impl_->context, voice_id);
}

/* ---- release all voice board notes --------------------------------------- */

voice_board_result_t voice_board_t::all_notes_off()
{
    if (!impl_ || !impl_->context) return unavailable();
    return voice_board_detail::all_notes_off(impl_->context);
}

/* ---- set voice board attenuation ----------------------------------------- */

voice_board_result_t voice_board_t::set_attenuation(uint8_t attenuation_db)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (attenuation_db > 127u)
        return invalid_argument("attenuation must be 0..127 dB");
    return voice_board_detail::set_attenuation(impl_->context, attenuation_db);
}
