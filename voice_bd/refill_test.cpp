/* Deterministic host/firmware credit integration. No OS or USB timing model. */
#include "voicebd.cpp"
#include "stream.h"
#include <cassert>
#include <deque>
using namespace voice_board_detail;

static void check_refill(unsigned delivery_ticks, unsigned status_ticks)
{
    stream_state host;
    host.samples[0].pcm.reset(new int16_t[1024]{});
    host.samples[0].pcm_size = 1024;
    init_voice_streams();
    std::array<int8_t, 1024> prime{};
    const unsigned rates[8] = {15, 30, 60, 120, 240, 360, 479, 480};
    for (unsigned v = 0; v < 8; ++v) {
        publish_pending_voice_stream(v, reserve_voice_stream(v, 0, 1));
        assert(write_stream_body_samples(v, 1, prime.data(), prime.size()) == 1);
        assert(activate_pending_voice_stream(v) == 0);
        auto &voice = host.voices[v];
        voice.active = true;
        voice.pending = false;
        voice.session_id = 1;
    }
    struct Packet {
        unsigned due;
        std::vector<uint8_t> bytes;
    };
    std::deque<Packet> wire;
    struct Status { unsigned due; card_status value; std::chrono::steady_clock::time_point requested; };
    std::deque<Status> replies;
    unsigned next_query = 0;
    uint32_t frames = 0;
    auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
    std::array<uint8_t, kUsbHeaderBytes + kUsbPayloadMax> packet;
    auto deliver = [&](unsigned tick) {
        while (!wire.empty() && wire.front().due <= tick) {
            const auto &bytes = wire.front().bytes;
            assert(bytes[0] == kBody && bytes[2] == 1);
            assert(write_stream_body_samples(bytes[1], bytes[2],
                reinterpret_cast<const int8_t *>(bytes.data() + kUsbHeaderBytes),
                read16(bytes.data() + 3)) == 1);
            wire.pop_front();
        }
    };
    for (unsigned tick = 0; tick < 20000; ++tick) {
        deliver(tick);
        card_status status;
        status.active_voice_mask = 255;
        status.buffer_capacity = STREAM_RING_SAMPLES;
        status.status_sequence = uint8_t(tick);
        status.last_usb_sequence = get_last_stream_body_sequence();
        for (unsigned v = 0; v < 8; ++v) {
            // Consume 5 ms at independently chosen supported source rates.
            assert(get_current_stream_buffered_samples(v) > rates[v]);
            assert(read_stream_samples(v, nullptr, rates[v]) == rates[v]);
            status.current_session_id[v] = status.session_id[v] = 1;
            status.free_samples[v] = get_stream_free_samples(v);
            status.refill_samples_5ms[v] = rates[v];
            unsigned remaining = (get_current_stream_buffered_samples(v) - 1) * 5000 / rates[v];
            status.remaining_us[v] = std::min(remaining / 100, 2047u) * 100;
        }
        if (tick >= next_query) {
            unsigned latency = tick >= 1000 ? status_ticks : 0;
            replies.push_back({tick + latency, status, now});
            next_query = tick + std::max(1u, latency);
        }
        bool status_applied = false;
        while (!replies.empty() && replies.front().due <= tick) {
            host.apply_status(replies.front().value, replies.front().requested);
            status_applied = true;
            replies.pop_front();
        }
        unsigned sent = 0;
        while (status_applied) {
            auto size = host.make_block(packet.data(), packet.size());
            if (!size) break;
            assert(++sent <= kPendingPacketCapacity);
            wire.push_back({tick + (tick >= 1000 ? delivery_ticks : 0),
                            std::vector<uint8_t>(packet.begin(), packet.begin() + size)});
            ++frames;
        }
        deliver(tick);
        now += std::chrono::milliseconds(5);
    }
    assert(frames > 65536); // Exercise both status and BODY acknowledgement wrap.
    assert(get_stream_dropped_block_count() == 0);
    printf("PASS deterministic refill: eight rates, 100 virtual seconds, delivery=%u ms, %u BODY frames\n",
           delivery_ticks * 5, frames);
    printf("  status delay=%u ms\n", status_ticks*5);
}
int main()
{
    check_refill(0, 0);
    check_refill(1, 0);
    check_refill(0, 2); // Existing ring capacity must cover delayed credit updates.
    check_refill(0, 4); // Same physical buffers and exact credit, 20 ms reply delay.
}
