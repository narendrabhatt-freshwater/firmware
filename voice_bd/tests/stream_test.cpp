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
    stream.voices[1].deadline = now + std::chrono::milliseconds(8);
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
    // A new voice keeps priority across packets and uses capacity left after
    // protecting a playing voice. No key or root frequency enters this test.
    for (unsigned rate : {12u,48u}) {
        stream_state startup;
        startup.samples[0].body_size=20000;
        startup.samples[0].body.reset(new int16_t[20000]{});
        for(auto v:{0u,1u}) { startup.voices[v].active=true; startup.voices[v].session_id=1; }
        startup.voices[1].filling_initial_buffer=true;
        auto q=status(1,0,3); q.pending_voice_mask=2;
        q.refill_samples_5ms[0]=rate*5; q.free_samples[0]=4080-rate*2;
        startup.apply_status(q,now);
        startup.make_packet(now);
        auto first = reinterpret_cast<uint8_t const*>(startup.packet.data());
        check((first[2]&7)==1 && read16(first+4)==998 && read16(first+8)==0,
              "new note gets its first full packet even when another voice needs data");
        for(unsigned ms=1;ms<4;++ms) {
            startup.make_packet(now+std::chrono::milliseconds(ms));
            auto bytes=reinterpret_cast<uint8_t const*>(startup.packet.data());
            check((bytes[2]&7)==0 && read16(bytes+4)==rate*(ms==1?2:1) &&
                  (bytes[6]&7)==1 && read16(bytes+8)==998-rate*(ms==1?2:1),
                  "playing demand is protected and new voice receives all spare capacity");
            check(startup.voices[1].filling_initial_buffer,
                  "startup priority survives sending the first packet");
        }
        q.status_sequence=2; q.last_usb_sequence=startup.next_sequence-1;
        q.pending_voice_mask=0; q.free_samples[1]=500; q.refill_samples_5ms[1]=3625;
        startup.apply_status(q,now+std::chrono::milliseconds(4));
        startup.make_packet(now+std::chrono::milliseconds(4));
        check(!startup.voices[1].filling_initial_buffer,
              "card-confirmed buffered demand ends startup priority");
    }
    // Paced USB delivery uses the acknowledged packet's audio age, not RTT.
    stream_state paced;
    paced.samples[0].body_size = 20000;
    paced.samples[0].body.reset(new int16_t[20000]{});
    paced.voices[0].active = true;
    paced.voices[0].session_id = 1;
    auto full = status(1, 0, 1);
    full.refill_samples_5ms[0] = 1920;
    paced.apply_status(full, now);
    paced.make_packet(now);
    auto const first_sequence = static_cast<uint16_t>(paced.next_sequence - 1u);
    full.status_sequence = 2;
    full.last_usb_sequence = first_sequence;
    full.free_samples[0] = 0;
    full.usb_age_ms = 0;
    paced.apply_status(full, now + std::chrono::milliseconds(15));
    for (unsigned ms = 1; ms <= 25; ++ms) {
        paced.make_packet(now + std::chrono::milliseconds(15 + ms * 2));
        auto n = read16(reinterpret_cast<uint8_t const*>(paced.packet.data()) + 4);
        check(n == (ms <= 2 ? 0 : 384),
              "USB packet cadence survives delayed replies and callback jitter");
    }
    full.status_sequence = 3; full.pending_voice_mask = 1;
    paced.apply_status(full, now + std::chrono::milliseconds(70));
    paced.make_packet(now + std::chrono::milliseconds(71));
    check(paced.voices[0].refill_samples_5ms == 0 && paced.voices[0].available_sample_slots == 0,
          "pending replacement cannot spend the old voice budget");
    // Aged snapshots must not count audio consumed before the snapshot twice.
    stream_state aged;
    aged.samples[0].body_size = 20000;
    aged.samples[0].body.reset(new int16_t[20000]{});
    aged.voices[0].active = true; aged.voices[0].session_id = 1;
    auto age_status = status(255, 0, 1);
    age_status.refill_samples_5ms[0] = 1920;
    aged.apply_status(age_status, now); aged.make_packet(now);
    age_status.status_sequence = 0; age_status.last_usb_sequence = 1;
    age_status.free_samples[0] = 0; age_status.usb_age_ms = 3;
    aged.apply_status(age_status, now + std::chrono::milliseconds(20));
    check(aged.last_status_sequence == 0, "eight-bit status sequence wraps");
    for(unsigned ms=1; ms<=6; ++ms) {
        aged.make_packet(now + std::chrono::milliseconds(30));
        auto n=read16(reinterpret_cast<uint8_t const*>(aged.packet.data())+4);
        check(n==(ms<=5?0:384), "card audio age removes pre-snapshot consumption");
    }
    // Model the card and USB pipe independently: status replies are delayed,
    // while packets enter the card four milliseconds after being generated.
    // Start with an empty transport pipe; allow 50 ms to establish steady flow.
    // Startup readiness is deliberately outside this steady-stream model.
    for (unsigned reply_delay : {2u, 9u, 15u})
    for (auto rates : {std::array<unsigned,8>{725,0,0,0,0,0,0,0},
                       std::array<unsigned,8>{600,300,0,0,0,0,0,0},
                       std::array<unsigned,8>{600,40,40,40,40,40,40,40}}) {
        stream_state model;
        model.samples[0].body_size = 500000;
        model.samples[0].body.reset(new int16_t[500000]{});
        std::array<int,8> fill{};
        uint8_t mask=0;
        for(unsigned v=0;v<8;++v) if(rates[v]) {
            mask |= 1u<<v; fill[v]=4080;
            model.voices[v].active=true; model.voices[v].session_id=1;
        }
        struct Delivery { unsigned at; std::array<int8_t,kUsbPacketByteCount> bytes; };
        struct Reply { unsigned at, requested; card_status value; };
        std::vector<Delivery> deliveries;
        std::vector<Reply> replies;
        uint16_t ack=0; unsigned ack_at=0; bool have_ack=false;
        unsigned seq=0, misses=0, overflows=0;
        for(unsigned ms=0;ms<300;++ms) {
            for(auto const& d:deliveries) if(d.at==ms) {
                auto bytes=reinterpret_cast<uint8_t const*>(d.bytes.data());
                if(read16(bytes+4)||read16(bytes+8)) {
                    ack=read16(bytes); ack_at=ms; have_ack=true;
                }
                for(unsigned block=0;block<2;++block) {
                    auto v=bytes[2+4*block]&7u; auto n=read16(bytes+4+4*block);
                    if(fill[v]+n>4080) ++overflows;
                    fill[v]+=n;
                }
            }
            for(unsigned v=0;v<8;++v) if(rates[v]) {
                if(ms>=50 && fill[v]<int(rates[v])) ++misses;
                fill[v]=std::max(0,fill[v]-int(rates[v]));
            }
            if(ms%5==0) {
                auto q=status(++seq & 255u,ack,mask);
                q.usb_age_ms=have_ack?std::min(ms-ack_at,255u):255u;
                for(unsigned v=0;v<8;++v) {
                    q.free_samples[v]=4080-fill[v]; q.refill_samples_5ms[v]=rates[v]*5;
                    q.remaining_us[v]=rates[v]?fill[v]*1000/rates[v]:0;
                }
                replies.push_back({ms+reply_delay,ms,q});
            }
            for(auto const& r:replies) if(r.at==ms)
                model.apply_status(r.value,now+std::chrono::milliseconds(r.requested));
            model.make_packet(now+std::chrono::milliseconds(ms));
            deliveries.push_back({ms+4,model.packet});
        }
        check(overflows==0,"pipeline accounting never overfills modeled rings");
        check(misses==0,"single and mixed-demand voices sustain delivery across polls");
    }
    // Packet assembly remains byte-identical across non-millisecond callbacks.
    stream_state fragmented;
    std::array<int8_t, 2016> output{};
    fragmented.render(output.data(), 21);
    fragmented.render(output.data() + 21, output.size() - 21);
    check(static_cast<uint8_t>(output[2]) == 255 &&
          static_cast<uint8_t>(output[1010]) == 255, "idle packets survive callback fragmentation");
    std::vector<uint8_t> raw(61, 0);
    raw[0] = 0xA5; raw[1] = 0x5A; raw[2] = 0x43; raw[3] = 12;
    raw[6] = 0xE0; raw[7] = 0x1F; raw[60] = '\n';
    card_status parsed;
    check(parse_voice_queue_status(raw, parsed), "compact budget status parses");
    auto record = [&](unsigned voice, unsigned session, unsigned free,
                      unsigned budget, unsigned duration) {
        size_t at = 12 + voice * 6;
        raw[at] = static_cast<uint8_t>(session);
        raw[at+1] = static_cast<uint8_t>(free);
        raw[at+2] = static_cast<uint8_t>((free >> 8) | ((budget & 7) << 5));
        raw[at+3] = static_cast<uint8_t>(budget >> 3);
        raw[at+4] = static_cast<uint8_t>(((budget >> 11) & 1) | ((duration & 127) << 1));
        raw[at+5] = static_cast<uint8_t>(duration >> 7);
    };
    raw[8] = 255; raw[9] = 17;
    check(parse_voice_queue_status(raw, parsed) && parsed.status_sequence == 255 &&
          parsed.usb_age_ms == 17, "sequence and USB age have independent bytes");
    raw[4] = 255;
    for(unsigned i=0;i<8;++i) record(i,i,100+i,(i+1)*480,1000*(i+1));
    check(parse_voice_queue_status(raw, parsed), "all eight packed records parse");
    for(unsigned i=0;i<8;++i)
        check(parsed.session_id[i]==i && parsed.free_samples[i]==100+i &&
              parsed.refill_samples_5ms[i]==(i+1)*480 &&
              parsed.remaining_us[i]==100000u*(i+1), "packed fields preserve all values");
    for(unsigned free : {0u,15u,16u,255u,256u,4079u,4080u,4095u,4096u,8159u,8160u})
        for(unsigned budget : {0u,15u,16u,255u,256u,3839u,3840u}) {
            record(0,0,free,budget,32767);
            check(parse_voice_queue_status(raw,parsed) && parsed.free_samples[0]==free &&
                  parsed.refill_samples_5ms[0]==budget && parsed.remaining_us[0]==3276700u,
                  "packed counts preserve shared nibbles and full duration precision");
        }
    record(0,0,8161,0,1);
    check(!parse_voice_queue_status(raw,parsed), "free space above capacity rejected");
    record(0,0,0,3841,1);
    check(!parse_voice_queue_status(raw,parsed), "demand above card limit rejected");
    record(0,255,0,0,1);
    check(!parse_voice_queue_status(raw,parsed), "active voice requires valid session");
    record(0,0,0,0,1);
    check(parse_voice_queue_status(raw,parsed) && parsed.remaining_us[0]==100,
          "duration retains tenth-millisecond resolution");
    raw.pop_back();
    check(!parse_voice_queue_status(raw,parsed), "partial reply cannot grant credit");
    raw.push_back('\n'); raw[3]=9;
    check(!parse_voice_queue_status(raw,parsed), "previous budget format rejected");
    raw.resize(53); raw[3]=8; raw[52]='\n';
    check(!parse_voice_queue_status(raw,parsed), "legacy reply rejected");
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
    device.next_poll = clock::now() - std::chrono::milliseconds(20);
    device.advance_poll();
    check(device.next_poll <= clock::now(), "late reply cannot postpone an already due poll");
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
