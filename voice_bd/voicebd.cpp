#include "voicebd.h"

/*
 * RS485 carries note and status commands. CDC is used for BEC and ATTACK
 * uploads. The sample BODY is streamed over USB audio.
 */

#include <RtAudio.h>
#include <libserialport.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
constexpr unsigned kUsbPacketHeaderByteCount = 4u;
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

/* ---- calculate the status crc8 ------------------------------------------- */

uint8_t crc8(uint8_t const* data, size_t size)
{
    uint8_t crc = 0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1u) ^ 0x07u)
            : static_cast<uint8_t>(crc << 1u);
    }
    return crc;
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
    uint8_t refill_voice = 0xFFu;
    uint16_t buffer_capacity = 0u;
    uint16_t status_sequence = 0u;
    uint16_t last_usb_sequence = 0u;
    std::array<uint8_t, kVoiceCount> session_id{};
    std::array<uint16_t, kVoiceCount> buffered_samples{};
    std::array<uint16_t, kVoiceCount> free_samples{};
};

/* ---- parse voice queue status -------------------------------------------- */

bool parse_voice_queue_status(std::vector<uint8_t> const& raw,
    card_status& status)
{
    for (size_t start = 0; start + 56u <= raw.size(); ++start) {
        uint8_t const* p = raw.data() + start;
        if (p[0] != 0xA5u || p[1] != 0x5Au || p[2] != 0x43u ||
            p[3] != 0x04u || p[55] != '\n' || p[54] != crc8(p, 54u)) continue;
        status.active_voice_mask = p[4];
        status.pending_voice_mask = p[5];
        status.refill_voice = p[6];
        status.buffer_capacity = read16(p + 8u);
        status.status_sequence = read16(p + 10u);
        status.last_usb_sequence = read16(p + 12u);
        if (status.buffer_capacity == 0u) return false;
        for (size_t i = 0; i < kVoiceCount; ++i) {
            size_t const at = 14u + i * 5u;
            status.session_id[i] = p[at];
            status.buffered_samples[i] = read16(p + at + 1u);
            status.free_samples[i] = read16(p + at + 3u);
            if (status.buffered_samples[i] > status.buffer_capacity ||
                status.free_samples[i] > status.buffer_capacity ||
                    status.buffered_samples[i] + status.free_samples[i] >
                        status.buffer_capacity) return false;
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
        for (unsigned attempt = 0; attempt < 3u; ++attempt) {
            port_.flush();
            std::string error;
            if (!port_.write(wire.data(), wire.size(), error))
                return fail(voice_board_error_t::io_error, error);
            std::vector<uint8_t> reply;
            auto const stop_waiting_at = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(500);
            std::array<uint8_t, 256> bytes{};
            while (std::chrono::steady_clock::now() < stop_waiting_at) {
                size_t const count = port_.read(
                    bytes.data(), bytes.size(), 40u);
                reply.insert(reply.end(), bytes.begin(), bytes.begin() + count);
                if (body == "vq") {
                    card_status status;
                    if (parse_voice_queue_status(reply, status)) {
                        if (binary) *binary = std::move(reply);
                        return ok();
                    }
                } else {
                    std::string const text(reply.begin(), reply.end());
                    size_t const tag = text.find('[');
                    size_t const end = text.find_first_of("\r\n", tag);
                    if (tag != std::string::npos && end != std::string::npos) {
                        std::string const line = text.substr(tag, end - tag);
                        if (line.find("err:") != std::string::npos)
                            return fail(voice_board_error_t::bad_reply, line);
                        return ok(line);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return fail(voice_board_error_t::timeout,
            "RS485 timeout waiting for Channel Card");
    }

};

/*******************************************************************************

                        u s b   a u d i o   s t r e a m

*******************************************************************************/

struct sample_data
{
    std::vector<int8_t> body;
    bool loaded = false;
};

struct voice_data
{
    bool active = false;
    bool waiting_for_first_packet = false;
    bool confirmed_by_card = false;
    uint8_t session_id = 0u;
    uint16_t sample_id = 0u;
    size_t body_position = 0u;
    unsigned available_sample_slots = 0u;
    unsigned buffered_sample_count = 0u;
};

/* A USB packet that the card has not acknowledged yet. */
struct pending_packet
{
    uint16_t sequence = 0u;
    uint8_t voice = 0u;
    uint16_t samples = 0u;
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
    uint8_t refill_voice = 0xFFu;
    uint8_t next_voice = 0u;

/* ---- apply voice queue status -------------------------------------------- */

    void apply_status(card_status const& status)
    {
        std::lock_guard<std::mutex> lock(mutex);
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
            if (!state.active ||
                (live_voice_mask & static_cast<uint8_t>(1u << voice)) == 0u ||
                    status.session_id[voice] != state.session_id) {
                state.available_sample_slots = 0u;
                continue;
            }
            state.confirmed_by_card = true;
            state.available_sample_slots =
                status.free_samples[voice] > samples_in_flight[voice]
                    ? status.free_samples[voice] - samples_in_flight[voice]
                    : 0u;
            state.buffered_sample_count =
                status.buffered_samples[voice] + samples_in_flight[voice];
            if (state.waiting_for_first_packet &&
                status.buffered_samples[voice] >= kUsbPacketBodySampleCount)
                state.waiting_for_first_packet = false;
        }
        refill_voice = status.refill_voice;
    }

/* ---- prepare the next usb audio packet ----------------------------------- */

    void make_packet()
    {
        std::fill(packet.begin(), packet.end(), 0);
        packet[0] = static_cast<int8_t>(kUsbIdlePacketTag);
        if (!sequence_ready ||
            pending_packet_count == kPendingPacketCapacity) {
            packet_offset = 0u;
            return;
        }
        auto const can_refill = [this](uint8_t voice) {
            return voice < kVoiceCount && voices[voice].active &&
                voices[voice].available_sample_slots >=
                kUsbPacketBodySampleCount;
        };
        uint8_t chosen = can_refill(refill_voice) ? refill_voice : 0xFFu;
        if (chosen == 0xFFu) {
            for (uint8_t offset = 0u; offset < kVoiceCount; ++offset) {
                uint8_t const voice = static_cast<uint8_t>(
                    (next_voice + offset) % kVoiceCount);
                if (!can_refill(voice)) continue;
                if (chosen == 0xFFu ||
                    voices[voice].buffered_sample_count <
                        voices[chosen].buffered_sample_count)
                    chosen = voice;
            }
        }
        if (chosen == 0xFFu) {
            packet_offset = 0u;
            return;
        }
        auto& voice = voices[chosen];
        auto& sample = samples[voice.sample_id];
        if (sample.body.empty()) {
            voice.active = false;
            packet_offset = 0u;
            return;
        }
        packet[0] = static_cast<int8_t>(kUsbPacketTag |
            (voice.waiting_for_first_packet ? kUsbStartOfVoiceFlag : 0u) |
            chosen);
        packet[1] = static_cast<int8_t>(voice.session_id);
        packet[2] = static_cast<int8_t>(next_sequence & 0xFFu);
        packet[3] = static_cast<int8_t>(next_sequence >> 8u);
        /* The packet is zero-filled: leave silence after the sample ends. */
        for (size_t i = 0; i < kUsbPacketBodySampleCount &&
                voice.body_position < sample.body.size(); ++i) {
            packet[kUsbPacketHeaderByteCount + i] =
                sample.body[voice.body_position++];
        }
        pending_packets[(first_pending_packet + pending_packet_count) %
            kPendingPacketCapacity] = {
                next_sequence, chosen,
                static_cast<uint16_t>(kUsbPacketBodySampleCount)};
        ++pending_packet_count;
        ++next_sequence;
        next_voice = static_cast<uint8_t>((chosen + 1u) % kVoiceCount);
        voice.available_sample_slots -= kUsbPacketBodySampleCount;
        voice.buffered_sample_count += kUsbPacketBodySampleCount;
        packet_offset = 0u;
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

/* ---- resample pcm to channel sample format ------------------------------- */

std::vector<int8_t> resample_i8(std::vector<int16_t> const& pcm,
    uint32_t source_rate)
{
    double const ratio = static_cast<double>(source_rate) / kSampleRate;
    size_t const output_size = static_cast<size_t>(
        std::ceil(static_cast<double>(pcm.size()) / ratio));
    std::vector<int8_t> output(output_size);
    for (size_t i = 0; i < output_size; ++i) {
        double const at = static_cast<double>(i) * ratio;
        size_t const left = std::min(static_cast<size_t>(at), pcm.size() - 1u);
        size_t const right = std::min(left + 1u, pcm.size() - 1u);
        double const fraction = at - static_cast<double>(left);
        double const value = pcm[left] + (pcm[right] - pcm[left]) * fraction;
        long const scaled = std::lround(value / 256.0);
        output[i] = static_cast<int8_t>(std::clamp<long>(scaled, -128, 127));
    }
    return output;
}

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
    stream_state stream;
    RtAudio audio;
    std::atomic<bool> connected{false};
    std::atomic<bool> worker_running{false};
    std::thread worker;

/* ---- shut down the channel device context -------------------------------- */

    ~device_context() { shutdown(); }

/* ---- start the usb audio stream ------------------------------------------ */

    voice_board_result_t start_audio()
    {
        if (audio.isStreamRunning()) return ok();
        unsigned selected = 0u;
        bool found = false;
        std::string available_devices;
        for (unsigned id : audio.getDeviceIds()) {
            RtAudio::DeviceInfo const info = audio.getDeviceInfo(id);
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
        RtAudioErrorType const opened = audio.openStream(
            &output, nullptr, RTAUDIO_SINT8, kSampleRate, &frames,
                [](void* out, void*, unsigned count, double,
                    RtAudioStreamStatus,
            void* user) -> int {
                static_cast<device_context *>(user)->stream.render(
                    static_cast<int8_t*>(out), count * kUsbAudioChannelCount);
                    return 0;
                }, this);
        if (opened != RTAUDIO_NO_ERROR)
            return fail(voice_board_error_t::audio_error, audio.getErrorText());
        RtAudioErrorType const started = audio.startStream();
        if (started != RTAUDIO_NO_ERROR) {
            std::string const error = audio.getErrorText();
            audio.closeStream();
            return fail(voice_board_error_t::audio_error, error);
        }
        return ok();
    }

/* ---- shut down the channel device ---------------------------------------- */

    void shutdown()
    {
        worker_running.store(false);
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
            worker.join();
        if (audio.isStreamRunning()) (void)audio.stopStream();
        if (audio.isStreamOpen()) audio.closeStream();
        if (rs485.is_open()) {
            (void)rs485.command("n off");
            rs485.close();
        }
        {
            std::lock_guard<std::mutex> lock(stream.mutex);
            for (auto& voice : stream.voices) voice = {};
            stream.first_pending_packet = 0u;
            stream.pending_packet_count = 0u;
            stream.sequence_ready = false;
            stream.packet_offset = kUsbPacketByteCount;
        }
        connected.store(false);
    }

/* ---- poll channel card status -------------------------------------------- */

    void poll()
    {
        while (worker_running.load()) {
            std::vector<uint8_t> raw;
            /* vq: voice queue status. */
            auto const result = rs485.command("vq", &raw);
            if (result) {
                card_status status;
                if (parse_voice_queue_status(raw, status)) {
                    stream.apply_status(status);
                    std::lock_guard<std::mutex> lock(stream.mutex);
                    uint8_t const live_voice_mask = static_cast<uint8_t>(
                        status.active_voice_mask | status.pending_voice_mask);
                    for (uint8_t voice = 0; voice < kVoiceCount; ++voice)
                        if (stream.voices[voice].confirmed_by_card &&
                            (live_voice_mask &
                                static_cast<uint8_t>(1u << voice)) == 0u)
                            stream.voices[voice].active = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    (void)state->rs485.command("e:ec 0"); /* Command echo off. */
    result = state->rs485.command(
        "g 1 " + std::to_string(config.initial_attenuation_db));
    if (!result) {
        state->shutdown();
        result.message = "Channel gain setup failed: " + result.message;
        return result;
    }
    result = state->rs485.command("n off");
    if (!result && result.message.find("no-program") == std::string::npos) {
        state->shutdown();
        result.message = "Channel silence failed: " + result.message;
        return result;
    }
    serial_port upload_port;
    std::string error;
    if (!upload_port.open(state->upload_usb_port_name, 115200u, true, error)) {
        state->shutdown();
        return fail(voice_board_error_t::io_error, error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::array<uint8_t, 256> drain{};
    while (upload_port.read(drain.data(), drain.size(), 5u) != 0u) {}
    for (uint8_t voice = 0u; voice < kVoiceCount; ++voice) {
        std::string const off = "c:n" + std::to_string(voice) + " off\r";
        if (!upload_port.write(off.data(), off.size(), error)) {
            state->shutdown();
            return fail(voice_board_error_t::io_error, error);
        }
        std::string ignored;
        (void)wait_for(upload_port, "ok", ignored, 1000u);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    for (uint8_t voice = 0u; voice < kVoiceCount; ++voice) {
        /* vmload: load the compiled program into one voice. */
        result = send_payload(upload_port,
            "vmload " + std::to_string(voice),
            bec.data(), bec.size(), "ok:vm");
        if (!result) {
            state->shutdown();
            result.code = voice_board_error_t::bec_error;
            result.message = "voice " + std::to_string(voice) +
                " BEC upload failed: " + result.message;
            return result;
        }
    }
    upload_port.close();
    /* Audio is opened on the first note. */
    state->connected.store(true);
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

/* ---- load a sample into the channel device ------------------------------- */

voice_board_result_t load_sample(device_context* state, uint16_t sample_id,
    std::vector<int16_t> const& pcm,
    uint32_t source_rate, double root_pitch_hz)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
    std::vector<int8_t> converted = resample_i8(pcm, source_rate);
    if (converted.empty())
        return fail(voice_board_error_t::sample_error,
            "sample is empty after resampling");
    size_t const attack_size = std::min<size_t>(
        kAttackSampleCount, converted.size());
    size_t const overlap = std::min<size_t>(
        kCrossfadeSampleCount, attack_size);
    size_t const body_start = attack_size - overlap;
    /* ATTACK is stored on card; BODY plays once over USB with a short overlap. */
    std::vector<int8_t> body(converted.begin() + body_start, converted.end());
    if (body.empty())
        return fail(voice_board_error_t::sample_error,
            "sample does not contain a BODY");
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        for (auto const& voice : state->stream.voices)
            if (voice.active && voice.sample_id == sample_id)
                return fail(voice_board_error_t::sample_error,
                    "sample is currently in use");
    }
    serial_port upload_port;
    std::string error;
    if (!upload_port.open(state->upload_usb_port_name, 115200u, true, error))
        return fail(voice_board_error_t::io_error, error);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    /* al: load the ATTACK data. */
    auto result = send_payload(
        upload_port, "al " + std::to_string(sample_id),
            reinterpret_cast<uint8_t const*>(converted.data()), attack_size,
            "ok:attack");
    upload_port.close();
    if (!result) {
        result.code = voice_board_error_t::sample_error;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        auto& sample = state->stream.samples[sample_id];
        sample.body = std::move(body);
        sample.loaded = true;
    }
    /* ar: set the pitch of the original sample. */
    result = state->rs485.command("ar " + std::to_string(sample_id) + " " +
        std::to_string(root_pitch_hz));
    return result ? ok("sample " + std::to_string(sample_id) + " loaded")
        : result;
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
    uint8_t session = 0u;
    {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        if (!state->stream.samples[sample].loaded)
            return fail(voice_board_error_t::sample_error,
                "sample " + std::to_string(sample) + " is not loaded");
        auto& slot = state->stream.voices[voice];
        session = static_cast<uint8_t>((slot.session_id + 1u) % kSessionIdWrap);
        slot = {};
        slot.active = true;
        slot.waiting_for_first_packet = true;
        slot.session_id = session;
        slot.sample_id = sample;
        slot.body_position = 0u;
    }
    result = state->rs485.command("aw " + std::to_string(voice) + " " +
        std::to_string(sample));
    if (result) {
        char const voice_char = static_cast<char>('0' + voice);
        std::string const note = std::string("n") + voice_char + " on " +
            std::to_string(key) + " " + std::to_string(velocity) + " @" +
                std::to_string(session);
        result = state->rs485.command(note);
    }
    if (!result) {
        std::lock_guard<std::mutex> lock(state->stream.mutex);
        state->stream.voices[voice].active = false;
        return result;
    }
    return ok("voice " + std::to_string(voice) + " started");
}

/* ---- release a channel voice --------------------------------------------- */

voice_board_result_t note_off(device_context* state, uint8_t voice)
{
    if (!device_is_open(state))
        return fail(voice_board_error_t::not_connected,
            "voice board is not connected");
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

/* ---- load a sample into the voice board ---------------------------------- */

voice_board_result_t voice_board_t::load_sample(
    uint16_t sample_id, std::vector<int16_t> const& pcm,
        uint32_t source_sample_rate_hz, double root_pitch_hz)
{
    if (!impl_ || !impl_->context) return unavailable();
    if (sample_id > 247u) return invalid_argument("sample ID must be 0..247");
    if (pcm.empty()) return invalid_argument("sample PCM is empty");
    if (source_sample_rate_hz == 0u)
        return invalid_argument("sample rate must be positive");
    if (!(root_pitch_hz > 0.0) || !std::isfinite(root_pitch_hz))
        return invalid_argument("root pitch must be positive and finite");
    return voice_board_detail::load_sample(
        impl_->context, sample_id, pcm, source_sample_rate_hz, root_pitch_hz);
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
