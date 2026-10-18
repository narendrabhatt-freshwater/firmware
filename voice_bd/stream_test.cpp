#include "voicebd.cpp"
#include <cassert>
using namespace voice_board_detail;
static void check_status_encoding() {
    unsigned cases = 0;
    for (unsigned sid : {0u, 1u, 127u, 254u}) {
        for (unsigned current : {0u, 2u, 128u, 254u}) {
            for (unsigned free : {0u, 16u, 255u, 256u, 1024u, 4080u}) {
                for (unsigned demand : {0u, 1u, 15u, 16u, 127u, 255u, 256u, 480u}) {
                    for (unsigned duration : {0u, 1u, 7u, 8u, 255u, 256u, 2047u}) {
                        std::vector<uint8_t> b(61);
                        b[0] = 0xa5;
                        b[1] = 0x5a;
                        b[2] = 0x43;
                        b[3] = 0x0e;
                        b[4] = b[5] = 255;
                        b[6] = 0xf0;
                        b[7] = 0x0f;
                        b[60] = '\n';
                        for (unsigned i = 0; i < 8; i++) {
                            auto *p = b.data() + 12 + i * 6;
                            p[0] = sid;
                            p[1] = current;
                            p[2] = free;
                            p[3] = (free >> 8) | ((demand & 15) << 4);
                            p[4] = (demand >> 4) | ((duration & 7) << 5);
                            p[5] = duration >> 3;
                        }
                        card_status s;
                        assert(parse_voice_queue_status(b, s));
                        for (unsigned i = 0; i < 8; i++) {
                            assert(s.session_id[i] == sid && s.current_session_id[i] == current &&
                                   s.free_samples[i] == free && s.refill_samples_5ms[i] == demand &&
                                   s.remaining_us[i] == duration * 100);
                        }
                        cases++;
                        b[3] = 0x0d;
                        assert(!parse_voice_queue_status(b, s));
                    }
                }
            }
        }
    }
    printf("PASS: %u packed status boundary combinations across all eight voices; incompatible "
           "schema rejected\n",
           cases);
}

// A silent note event may precede publication by the audio owner. Status
// snapshots from before that publication must neither cancel nor feed the note.
static void check_delayed_session_publication() {
    stream_state stream;
    stream.samples[0].pcm.reset(new int16_t[2048]{});
    stream.samples[0].pcm_size = 2048;
    auto &target = stream.voices[0];
    target.active = true;
    target.session_id = 9;
    target.initial_samples_left = 998;
    card_status status;
    status.buffer_capacity = 4080;
    status.current_session_id.fill(255);
    status.session_id.fill(255);
    status.free_samples[0] = 4080;
    auto now = std::chrono::steady_clock::now();
    std::array<uint8_t, 1029> packet;
    for (unsigned i = 0; i < 100; ++i) {
        ++status.status_sequence;
        now += std::chrono::seconds(1);
        stream.apply_status(status, now);
        assert(target.active && !target.note_seen_in_status);
        assert(stream.make_block(packet.data()) == 0);
        assert(target.source_position == 0 && target.initial_samples_left == 998);
    }
    status.pending_voice_mask = 1;
    status.session_id[0] = 9;
    ++status.status_sequence;
    stream.apply_status(status, now);
    assert(target.active && target.note_seen_in_status && target.pending);
    assert(stream.make_block(packet.data()) > 5 && packet[2] == 9);
    puts("PASS: delayed session publication retains silent note without premature BODY or expiry");
}

static void check_packet_sample_boundaries() {
    stream_state stream;
    auto &sample = stream.samples[0];
    sample.pcm.reset(new int16_t[65536]);
    for (unsigned i = 0; i < 65536; ++i) sample.pcm[i] = int16_t(i);
    auto &voice = stream.voices[0];
    voice.active = voice.note_seen_in_status = true;
    voice.pending = false;
    stream.sequence_ready = true;
    uint8_t out[1029];
    // Boundary and signed PCM checks, including a cursor beyond a replaced sample.
    for (size_t length : {size_t(0), size_t(1), size_t(127), size_t(128), size_t(129), size_t(65536)}) {
        sample.pcm_size = length;
        for (size_t position : {size_t(0), size_t(1), size_t(126), size_t(65500), size_t(65536), size_t(70000)}) {
            for (unsigned capacity : {6u, 20u, 133u, 1029u}) {
                voice.source_position = position;
                stream.pool_credit[0] = 4080;
                stream.pending_packet_count = 0;
                size_t n = stream.make_block(out, capacity);
                if (!length) {
                    assert(n == 0);
                    continue;
                }
                assert(n == std::min(capacity, 133u));
                size_t pos = position;
                for (size_t i = 5; i < n;++i) {
                    uint8_t expected = pos < length ? uint8_t(sample.pcm[pos++] >> 8) : 0;
                    assert(out[i] == expected);
                }
                assert(voice.source_position == pos);
            }
        }
    }
    puts("PASS: BODY PCM conversion, sample end, zero tail and replacement cursor boundaries");
}

int main() {
    check_packet_sample_boundaries();
    check_delayed_session_publication();
    check_status_encoding();
    stream_state stream;
    stream.samples[0].pcm.reset(new int16_t[10000]);
    stream.samples[0].pcm_size = 10000;
    for (unsigned i = 0; i < 10000; i++)
        stream.samples[0].pcm[i] = int16_t((i % 100) * 256);
    auto now = std::chrono::steady_clock::now();
    auto &target = stream.voices[0];
    target.active = target.note_seen_in_status = true;
    target.pending = false;
    target.session_id = 1;
    target.source_position = 1000;
    card_status status;
    status.buffer_capacity = 4080;
    status.current_session_id.fill(255);
    status.session_id.fill(255);
    status.active_voice_mask = 1;
    status.current_session_id[0] = status.session_id[0] = 1;
    status.free_samples[0] = 2048;
    status.remaining_us[0] = 1000;
    status.refill_samples_5ms[0] = 480;
    status.status_sequence = 1;
    stream.apply_status(status, now);
    stream.history[0][1] = target;
    target = {};
    target.active = true;
    target.session_id = 2;
    target.source_position = 480;
    target.initial_samples_left = 998;
    target.deadline = now + std::chrono::milliseconds(5);
    status.pending_voice_mask = 1;
    status.session_id[0] = 2;
    status.status_sequence++;
    now += std::chrono::milliseconds(1);
    stream.apply_status(status, now);
    std::array<uint8_t, 1029> packet;
    unsigned old = 0, next = 0, charged = 0, blocks = 0;
    while (auto n = stream.make_block(packet.data())) {
        unsigned count = read16(packet.data() + 3);
        assert(n == count + 5 && count <= 128);
        charged += (count + 15) / 16 * 16;
        blocks++;
        if (packet[2] == 1)
            old += count;
        else {
            assert(packet[2] == 2);
            next += count;
        }
    }
    assert(next > 0 && next <= 998 && old > 0);
    assert(target.initial_samples_left == 998 - next);
    assert(charged == 2048); // Use granted capacity without starving either session.
    assert(stream.pool_credit[0] == 2048 - charged);
    assert(stream.history[0][1].source_position == 1000 + old);
    assert(stream.voices[0].source_position == 480 + next);
    status.pending_voice_mask = 0;
    status.current_session_id[0] = 2;
    status.last_usb_sequence = blocks;
    status.free_samples[0] = 128;
    status.remaining_us[0] = 0;
    status.status_sequence++;
    now += std::chrono::milliseconds(5);
    stream.apply_status(status, now);
    assert(stream.current_voice(0) == &target);
    assert(stream.make_block(packet.data()) == 133);
    assert(packet[2] == 2 && packet[5] == (480 + next) % 100);
    stream.history[0][2] = target;
    target = {};
    target.active = true;
    target.session_id = 3;
    target.source_position = 480;
    target.initial_samples_left = 998;
    status.last_usb_sequence++;
    status.status_sequence++;
    now += std::chrono::milliseconds(5);
    stream.apply_status(status, now);
    assert(stream.make_block(packet.data()) == 133 && packet[2] == 2);
    auto credit = stream.pool_credit[0];
    stream.apply_status(status, now);
    assert(stream.pool_credit[0] == credit);
    puts("PASS: outgoing and replacement cursors, shared rounded credit, promotion, cancellation "
         "and duplicate-status rejection");
}
