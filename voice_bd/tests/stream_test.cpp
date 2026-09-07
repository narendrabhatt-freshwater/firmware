// Standalone test translation unit: compile this file, not voicebd.cpp separately.
// Including the implementation gives tests access to internal scheduler state
// without adding test hooks or exposing internals in the production API.
#include "../voicebd.cpp"
#include <iostream>
#include <stdexcept>

namespace voice_board_detail {
int stream_self_test()
{
    auto check = [](bool value, char const* what) {
        if (!value) throw std::runtime_error(what);
    };
    using clock = std::chrono::steady_clock;
    auto const now = clock::now();
    stream_state stream;
    stream.samples[0].body_size = 20000;
    stream.samples[0].body.reset(new int16_t[20000]);
    std::fill_n(stream.samples[0].body.get(), 20000, 37 * 256);
    auto arm = [&](unsigned v, unsigned session = 1u) {
        stream.voices[v] = {};
        stream.voices[v].active = true;
        stream.voices[v].session_id = static_cast<uint8_t>(session);
        stream.voices[v].waiting_for_first_packet = true;
    };
    auto status = [&](uint16_t seq, uint16_t ack, uint8_t live) {
        card_status s;
        s.buffer_capacity = 4080;
        s.status_sequence = seq;
        s.last_usb_sequence = ack;
        s.active_voice_mask = live;
        s.session_id.fill(1);
        s.free_samples.fill(4080);
        s.remaining_us.fill(20000);
        return s;
    };
    auto block_count = [&](unsigned i) {
        return read16(reinterpret_cast<uint8_t const*>(stream.packet.data()) + 4u + i * 4u);
    };
    stream.samples[0].body[0] = -32768;
    stream.samples[0].body[1] = 32767;
    stream.samples[0].body[2] = -1;
    auto idle_status = status(0, 65534, 0);
    stream.apply_status(idle_status, now);
    check(stream.voices[0].available_sample_slots == 4080,
          "idle voice retains free space confirmed by vq for its next note");
    stream.make_packet(now);
    check(block_count(0) == 0 && block_count(1) == 0,
          "known free space does not send samples before note authorization");
    arm(0);
    auto s = status(1, 65534, 1);
    stream.apply_status(s, now);
    for (unsigned i = 0; i < 4; ++i) {
        stream.make_packet(now);
        check(block_count(0) == 998 && block_count(1) == 0, "single voice continuous fill");
        if (i == 0u)
            check(stream.packet[10] == -128 && stream.packet[11] == 127 &&
                  stream.packet[12] == -1 && stream.packet[13] == 37,
                  "packet fill quantizes retained PCM exactly to signed eight-bit samples");
    }
    stream.make_packet(now);
    check(block_count(0) == 88 && stream.voices[0].available_sample_slots == 0,
          "partial tail fills exact capacity");
    stream.make_packet(now);
    check(block_count(0) == 0 && block_count(1) == 0, "full ring emits idle");
    check(stream.pending_packet_count == 5, "idle never enters in-flight ledger");
    s.status_sequence = 2;
    s.last_usb_sequence = 0; // first two packets processed, including wrap
    s.free_samples[0] = 2200;
    stream.apply_status(s, now);
    check(stream.voices[0].available_sample_slots == 116,
          "snapshot subtracts only later packets across sequence wrap");
    s.free_samples[0] = 4080;
    stream.apply_status(s, now);
    check(stream.voices[0].available_sample_slots == 116, "duplicate status cannot grant space");
    arm(0, 2); // prior-session packets remain in flight
    s.status_sequence = 3; s.session_id[0] = 2; s.pending_voice_mask = 1;
    stream.apply_status(s, now);
    check(stream.voices[0].available_sample_slots == 1996 && stream.voices[0].pending,
          "replacement accounts for outstanding prior-session data");
    s.status_sequence = 4; s.session_id[0] = 1;
    stream.apply_status(s, now);
    check(stream.voices[0].available_sample_slots == 0, "wrong session grants no space");
    arm(0); arm(1);
    s = status(5, 3, 3); s.remaining_us[0] = 3000; s.remaining_us[1] = 4000;
    stream.apply_status(s, now);
    stream.make_packet(now);
    check(block_count(0) == 499 && block_count(1) == 499 &&
          (stream.packet[2] & 7) == 0 && (stream.packet[6] & 7) == 1,
          "two urgent voices split payload by earliest deadline");
    stream.voices[0].available_sample_slots = 12;
    stream.make_packet(now);
    check(block_count(0) == 12 && block_count(1) == 986,
          "unused half transfers to other urgent voice");
    stream.voices[0].available_sample_slots = 1000;
    stream.voices[1].available_sample_slots = 1000;
    stream.voices[0].deadline = now + std::chrono::milliseconds(8);
    stream.voices[1].deadline = now + std::chrono::milliseconds(9);
    stream.make_packet(now);
    check(block_count(0) == 499 && block_count(1) == 499,
          "refill both ATTACK deadlines early enough for polling and USB delivery");
    stream.voices[0].available_sample_slots = 998;
    stream.voices[0].initial_samples_left = 998;
    stream.voices[0].deadline = now + std::chrono::milliseconds(20);
    stream.voices[1].available_sample_slots = 998;
    stream.voices[1].deadline = now;
    stream.make_packet(now);
    check((stream.packet[2] & 7) == 0 && block_count(0) == 998 &&
          block_count(1) == 0 && stream.voices[0].initial_samples_left == 0,
          "new note receives its full first payload before shared refills");
    stream.voices[0].available_sample_slots = 12;
    stream.voices[0].initial_samples_left = 998;
    stream.make_packet(now);
    check(block_count(0) == 12 && block_count(1) == 986 &&
          stream.voices[0].initial_samples_left == 986,
          "partial initial credit keeps priority and gives spare payload space to another voice");
    stream.voices[0].initial_samples_left = 0;
    arm(2);
    s = status(6, 5, 7); s.pending_voice_mask = 4;
    stream.apply_status(s, now);
    stream.make_packet(now);
    check((stream.packet[2] & 7) == 2 && block_count(1) == 0,
          "new note precedes nonurgent filling");
    s = status(7, 6, 7); s.pending_voice_mask = 4; s.remaining_us[0] = 1000;
    stream.apply_status(s, now);
    stream.make_packet(now);
    check(((1u << (stream.packet[2] & 7)) | (1u << (stream.packet[6] & 7))) == 5u,
          "new note shares with urgent playing voice");
    for (unsigned i = 0; i < 8; ++i) arm(i);
    s = status(8, static_cast<uint16_t>(stream.next_sequence - 1u), 255); s.remaining_us.fill(8000);
    stream.apply_status(s, now);
    unsigned seen = 0;
    for (unsigned i = 0; i < 4; ++i) {
        stream.make_packet(now + std::chrono::milliseconds(3));
        check(block_count(0) == 499 && block_count(1) == 499, "deadline counts down between replies");
        seen |= 1u << (stream.packet[2] & 7);
        seen |= 1u << (stream.packet[6] & 7);
    }
    check(seen == 255, "equal deadlines rotate fairly across eight voices");
    s = status(9, static_cast<uint16_t>(stream.next_sequence - 1u), 255);
    for (unsigned i = 0; i < 8; ++i) s.remaining_us[i] = (i + 1u) * 1000u;
    stream.apply_status(s, now);
    seen = 0;
    for (unsigned i = 0; i < 4; ++i) {
        stream.make_packet(now);
        seen |= 1u << (stream.packet[2] & 7);
        seen |= 1u << (stream.packet[6] & 7);
    }
    check(seen == 255,
          "urgent voices without queued BODY are served before repeatedly filling earlier deadlines");
    s = status(10, static_cast<uint16_t>(stream.next_sequence - 1u), 0);
    stream.apply_status(s, now);
    stream.make_packet(now);
    check(block_count(0) == 0 && !stream.voices[0].active, "retired voices stop streaming");
    // Packet assembly remains byte-identical across non-millisecond callbacks.
    stream_state fragmented;
    std::array<int8_t, 2016> output{};
    fragmented.render(output.data(), 21);
    fragmented.render(output.data() + 21, output.size() - 21);
    check(static_cast<uint8_t>(output[2]) == 255 &&
          static_cast<uint8_t>(output[1010]) == 255, "idle packets survive callback fragmentation");
    std::vector<uint8_t> raw(53, 0);
    raw[0] = 0xA5; raw[1] = 0x5A; raw[2] = 0x43; raw[3] = 8;
    raw[6] = 0xF0; raw[7] = 0x0F; raw[52] = '\n';
    card_status parsed;
    check(parse_voice_queue_status(raw, parsed), "new status parses without CRC");
    raw[4] = 255;
    for (unsigned i = 0; i < 8; ++i) {
        size_t at = 12u + i * 5u;
        raw[at] = static_cast<uint8_t>(i);
        raw[at+1] = static_cast<uint8_t>(100u+i);
        uint16_t duration = static_cast<uint16_t>(1000u * (i+1u));
        for (unsigned j = 0; j < 2; ++j)
            raw[at+3+j] = static_cast<uint8_t>(duration >> (8u*j));
    }
    check(parse_voice_queue_status(raw, parsed), "all-voice status parses");
    for (unsigned i = 0; i < 8; ++i)
        check(parsed.session_id[i]==i && parsed.free_samples[i]==100u+i &&
              parsed.remaining_us[i]==100000u*(i+1u), "all eight durations and credits survive wire decoding");
    raw[15] = 255; raw[16] = 255;
    check(parse_voice_queue_status(raw, parsed) && parsed.remaining_us[0] == 6553500u,
          "compact duration decodes full range without overflow");
    raw[15] = 1; raw[16] = 0;
    check(parse_voice_queue_status(raw, parsed) && parsed.remaining_us[0] == 100u,
          "duration preserves tenth-millisecond resolution");
    auto truncated = raw;
    truncated.pop_back();
    check(!parse_voice_queue_status(truncated, parsed), "partial status never grants space");
    raw[3] = 7;
    check(!parse_voice_queue_status(raw, parsed), "invalid status header rejected");
    raw[3] = 8; raw[13] = 255; raw[14] = 255;
    check(!parse_voice_queue_status(raw, parsed), "invalid free-space count rejected");
    // A queued note-off advertises priority while a query owns the control lock.
    device_context device;
    auto const scheduled_poll = device.next_poll;
    std::unique_lock<std::mutex> query_lock(device.control_mutex);
    std::atomic<bool> command_ran{false};
    std::thread note_off_turn([&] {
        device_context::control_turn turn(&device);
        command_ran.store(true);
    });
    auto const limit = clock::now() + std::chrono::seconds(2);
    while (device.waiting_commands.load() == 0u && clock::now() < limit)
        std::this_thread::yield();
    bool const queued = device.waiting_commands.load() == 1u && !command_ran.load();
    query_lock.unlock();
    note_off_turn.join();
    check(queued && command_ran.load() && device.waiting_commands.load() == 0u,
          "note-off claims the next control turn after the current query");
    check(device.next_poll == scheduled_poll,
          "note-off does not unnecessarily postpone the next status query");
    {
        device_context::control_turn turn(&device);
    }
    check(device.next_poll == scheduled_poll,
          "note-on keeps the regular polling schedule");
    return 0;
}
} // namespace voice_board_detail

int main()
{
    try {
        int result = voice_board_detail::stream_self_test();
        std::cout << "voicebd streaming tests passed\n";
        return result;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
