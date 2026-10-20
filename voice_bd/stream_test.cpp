#include "voicebd.cpp"
#include <cassert>
using namespace voice_board_detail;

/* ---- exercise every status field and reject incompatible framing -------- */

static void check_status_encoding()
{
    unsigned cases = 0;
    for (unsigned session : {0u, 1u, 127u, 254u, 255u})
    {
        for (unsigned free : {0u, 16u, 255u, 256u, 1024u, 4080u})
        {
            for (unsigned demand : {0u, 1u, 15u, 16u, 127u, 255u, 256u, 480u})
            {
                std::vector<uint8_t> bytes(56);
                bytes[0] = 0xa5;
                bytes[1] = 0x5a;
                bytes[2] = 0x43;
                bytes[3] = 0x0f;
                bytes[4] = 255;
                write16(bytes.data() + 5, 65535);
                bytes[55] = '\n';
                for (unsigned voice = 0; voice < 8; ++voice)
                {
                    auto *record = bytes.data() + 7 + voice * 6;
                    record[0] = voice;
                    record[1] = session;
                    write16(record + 2, free);
                    write16(record + 4, demand);
                }
                card_status status;
                assert(parse_voice_queue_status(bytes, status));
                assert(status.sequence == 65535 && status.active_voice_mask == 255);
                for (unsigned voice = 0; voice < 8; ++voice)
                {
                    assert(status.session_id[voice] == session);
                    assert(status.free_samples[voice] == free);
                    assert(status.refill_samples_5ms[voice] == demand);
                }
                ++cases;
                bytes[3] = 0x0e;
                assert(!parse_voice_queue_status(bytes, status));
                bytes[3] = 0x0f;
                bytes[7] = 9;
                assert(!parse_voice_queue_status(bytes, status));
            }
        }
    }
    printf("PASS: %u compact status combinations, sequence wrap value and invalid schemas\n", cases);
}

/* ---- preserve replacement cursors and credits across delayed BODY delivery */

int main()
{
    check_status_encoding();
    stream_state stream;
    stream.samples[0].pcm.reset(new int16_t[10000]);
    stream.samples[0].pcm_size = 10000;
    for (unsigned i = 0; i < 10000; ++i)
    {
        stream.samples[0].pcm[i] = int16_t((i % 100) * 256);
    }
    auto now = std::chrono::steady_clock::now();
    auto &target = stream.voices[0];
    target.active = true;
    target.note_seen_in_status = true;
    target.pending = false;
    target.session_id = 1;
    target.source_position = 1000;
    card_status status;
    status.session_id.fill(255);
    status.active_voice_mask = 1;
    status.session_id[0] = 1;
    status.free_samples.fill(kBufferCapacity);
    status.free_samples[0] = 4000;
    status.refill_samples_5ms[0] = 480;
    status.sequence = 65534;
    stream.apply_status(status, now);
    stream.history[0][1] = target;
    target = {};
    target.active = true;
    target.session_id = 2;
    target.source_position = 480;
    target.stream_origin = 480;
    target.initial_samples_left = 998;
    target.deadline = now + std::chrono::milliseconds(5);
    now += std::chrono::milliseconds(1);
    stream.apply_status(status, now);
    std::array<uint8_t, 1029> packet;
    unsigned old = 0;
    unsigned next = 0;
    unsigned charged = 0;
    unsigned blocks = 0;
    while (auto size = stream.make_block(packet.data(), now))
    {
        unsigned count = read16(packet.data() + 3);
        assert(size == count + 5 && count <= 128 && count % 16 == 0);
        charged += count;
        ++blocks;
        if (packet[2] == 1)
        {
            old += count;
        }
        else
        {
            assert(packet[2] == 2);
            next += count;
        }
    }
    assert(next == 496 && old > 0);
    assert(stream.poll_interval_ms() == 5);
    assert(stream.pool_credit[0] == 4000 - charged);
    assert(stream.history[0][1].source_position == 1000 + old);
    assert(target.source_position == 480 + next);
    now += std::chrono::milliseconds(1);
    stream.apply_status(status, now);
    assert(stream.pool_credit[0] == 4000 - charged);
    assert(stream.pending_packet_count == blocks);
    status.sequence = uint16_t(65534 + blocks);
    status.free_samples[0] = 4000 - charged;
    now += std::chrono::milliseconds(1);
    stream.apply_status(status, now);
    assert(stream.pending_packet_count == 0);
    assert(target.acknowledged_samples == next);
    assert(stream.pool_credit[0] == status.free_samples[0]);
    status.session_id[0] = 2;
    status.free_samples[0] = 4000;
    now += std::chrono::milliseconds(5);
    stream.apply_status(status, now);
    assert(stream.current_voice(0) == &target && !target.pending);
    assert(stream.make_block(packet.data(), now) == 133);
    assert(packet[2] == 2 && packet[5] == (480 + next) % 100);
    unsigned credit = stream.pool_credit[0];
    stream.apply_status(status, now);
    assert(stream.pool_credit[0] == credit);
    status.active_voice_mask = 0;
    status.session_id[0] = 255;
    now += std::chrono::milliseconds(5);
    stream.apply_status(status, now);
    assert(!target.active);
    puts("PASS: replacement cursors, in-flight credit, sequence wrap, promotion, duplicate status and release");
}
