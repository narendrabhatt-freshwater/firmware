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

#include "backend_model.h"
#include "audio.h"
#include "channel.h"
#include "voice.h"
#include "usb.h"
#include "stream.h"
#include "samples.h"
#ifndef LEGACY_API
#include "transport.h"
// Test-only interleaving: publish one BODY sample just before each tail peek.
static unsigned tail_arrivals, tail_peeks;
static int interleaved_tail_peek(uint8_t, uint32_t, int8_t *);
extern "C" {
#include "berry_backend.h"
}
#include <cassert>
static bool inject_clear_after_publication;
static void interleaved_boundary_begin(ScriptBerryRuntime *);
#define peek_stream_sample interleaved_tail_peek
#define script_berry_boundary_begin interleaved_boundary_begin
#include "voice.cpp"
#undef script_berry_boundary_begin
#undef peek_stream_sample
static void interleaved_boundary_begin(ScriptBerryRuntime *runtime)
{
    script_berry_boundary_begin(runtime);
    if (inject_clear_after_publication) {
        inject_clear_after_publication = false;
        assert(queue_streamed_sample_note(0, 0, 64, 127, 23) == 0);
        request_stop_all_voices();
    }
}
static int interleaved_tail_peek(uint8_t voice, uint32_t offset, int8_t *out)
{
    ++tail_peeks;
    if (tail_arrivals) {
        int8_t sample = 2;
        if (write_stream_body_samples(voice, 9, &sample, 1) != 1) abort();
        --tail_arrivals;
    }
    return peek_stream_sample(voice, offset, out);
}
#include "platform.h"
#endif
#include <assert.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

using namespace model;
static void frame(uint8_t type, std::vector<uint8_t> data={}, uint8_t target=0, uint8_t session=0)
{
    uint8_t head[]={type,target,session,(uint8_t)data.size(),(uint8_t)(data.size()>>8)};
    usb_rx.insert(usb_rx.end(),head,head+5);
    usb_rx.insert(usb_rx.end(),data.begin(),data.end());
}
static void service(unsigned n=4)
{
    while(n--) process_usb_messages();
}
static void hello()
{
    reconnect(); service();
    frame(USB_MSG_HELLO,{USB_STREAM_VERSION}); service();
    assert(usb_tx.size()==19 && usb_tx[0]==USB_MSG_REPLY && usb_tx[5]==0);
    usb_tx.clear();
}
static void command(const char *text)
{
    rs_rx.insert(rs_rx.end(),text,text+strlen(text));
#ifdef LEGACY_API
    process_channel_commands_and_leds();
#else
    process_rs485_commands();
#endif
    assert(!bus_owned);
}
#ifndef LEGACY_API
static unsigned entered, restored, barriers, gains, colours;
static uint32_t overlaps = 7;
static bool gain_ready = true;
static uint32_t enter() { ++entered; return 0x1234u; }
static void leave(uint32_t saved) { assert(saved == 0x1234u); ++restored; }
static void barrier() { ++barriers; }
static uint32_t read_overlaps() { return overlaps; }
static void clear_overlaps() { overlaps = 0; }
static int colour(float r, float g, float b, float brightness) {
    assert(r == 1 && g == 0.5f && b == 0 && brightness == 0.25f);
    ++colours; return 0;
}
static int gain(char channel, uint8_t half_db) {
    assert(channel == '1' && half_db == 24);
    ++gains; return gain_ready ? 0 : -1;
}
static void optional_services()
{
    // No physical outputs or synchronization backend is required by a host.
    assert(set_channel_led_colour(1, 0.5f, 0, 0.25f) == 0);
    rs_tx.clear(); command("c:g 1 12\r");
    assert(std::string(rs_tx.begin(), rs_tx.end()) == "[C] ok\r\n");
    static const ChannelOutputs outputs = {colour, gain};
    set_channel_outputs(&outputs);
    assert(set_channel_led_colour(1, 0.5f, 0, 0.25f) == 0 && colours == 1);
    command("c:g 1 12\r"); assert(gains == 1);
    gain_ready = false; rs_tx.clear(); command("c:g 1 12\r");
    assert(std::string(rs_tx.begin(), rs_tx.end()) == "[C] err:range\r\n");
    command("c:g 9 12\r"); assert(gains == 2); // Validate before dispatch.
    set_channel_outputs(nullptr);

    static const PlatformSynchronization sync = {enter, leave, barrier};
    set_platform_synchronization(&sync);
    command("c:vq\r"); assert(entered == 1 && restored == 1);
    board_memory_barrier(); assert(barriers == 1);
    set_platform_synchronization(nullptr);
    assert(disable_interrupts_and_save_state() == 0);
    restore_interrupt_state(0); board_memory_barrier();
    assert(entered == 1 && restored == 1 && barriers == 1);

    assert(get_audio_refill_overlap_count() == 0);
    static const AudioOutputDiagnostics diagnostics = {read_overlaps, clear_overlaps};
    set_audio_output_diagnostics(&diagnostics);
    assert(get_audio_refill_overlap_count() == 7);
    clear_audio_stream_statistics(); assert(overlaps == 0);
    set_audio_output_diagnostics(nullptr);
}
#endif
#ifndef LEGACY_API
static void check_script_upload(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> program((std::istreambuf_iterator<char>(file)), {});
    assert(program.size() > 20);
    auto begin = [&] {
        usb_tx.clear();
        std::vector<uint8_t> request = {USB_UPLOAD_SCRIPT};
        for (unsigned i = 0; i < 4; ++i) request.push_back(uint8_t(program.size() >> (8 * i)));
        frame(USB_MSG_UPLOAD_BEGIN, request); service();
    };
    auto result = [&](bool success) {
        assert(usb_tx.size() >= 6 && usb_tx[0] == USB_MSG_REPLY);
        assert((usb_tx[5] == 0) == success);
        usb_tx.clear();
    };

    hello();
    // MAS clears playback immediately before loading a patch. The clear reply
    // precedes the audio boundary; BEGIN must retain the request until then.
    note_active[0] = 1;
    publish_voice_status();
    request_stop_all_voices();
    begin();
    assert(usb_tx.empty() && !is_voice_script_upload_busy());
    int32_t audio[48];
    render_audio_samples(audio, 48);
    service(); result(true);
    frame(USB_MSG_UPLOAD_ABORT); service(); result(true);
    // A busy renderer leaves the complete BEGIN in the existing parser.
    assert(begin_audio_runtime());
    begin();
    assert(usb_tx.empty() && !is_voice_script_upload_busy());
    end_audio_runtime(); service(); result(true);
    assert(is_voice_script_upload_busy());
    frame(USB_MSG_UPLOAD_DATA, {1, 0, 0, 0, 42}); service(); result(false);
    assert(!is_voice_script_upload_busy()); // Bad offset releases the VM.

    begin(); result(true);
    frame(USB_MSG_UPLOAD_ABORT); service(); result(true);
    assert(!is_voice_script_upload_busy());
    begin(); result(true);
    reconnect(); service();
    assert(!is_voice_script_upload_busy());

    hello(); begin(); result(true);
    std::vector<uint8_t> bad_header(24, 0); // offset zero + invalid FWSC header
    frame(USB_MSG_UPLOAD_DATA, bad_header); service(); result(false);
    assert(!is_voice_script_upload_busy());

    begin(); result(true);
    for (size_t offset = 0; offset < program.size();) {
        size_t count = std::min<size_t>(17, program.size() - offset);
        std::vector<uint8_t> data;
        for (unsigned i = 0; i < 4; ++i) data.push_back(uint8_t(offset >> (8 * i)));
        data.insert(data.end(), program.begin() + offset, program.begin() + offset + count);
        frame(USB_MSG_UPLOAD_DATA, data); service(); result(true);
        offset += count;
        assert(bool(is_voice_script_upload_busy()) == (offset < program.size()));
    }
    assert(is_voice_script_active(0));
    // Old text-upload commands keep their existing error on the RS485 wire.
    for (const char *text : {"c:al 0 8\r", "c:wl 0 8\r", "c:vmload 0 20\r"}) {
        rs_tx.clear(); command(text);
        assert(std::string(rs_tx.begin(), rs_tx.end()) == "[C] err:usb\r\n");
    }
}
#endif

int main(int argc, char **argv)
{
    init_usb_protocol(); init_channel_card();
#ifndef LEGACY_API
    if (argc == 3 && strcmp(argv[2], "upload-errors") == 0) {
        check_script_upload(argv[1]);
        return 0;
    }
    model::connected = true; ++model::epoch; service();
    note_active[0] = 1; publish_voice_status();
    model::connected = false; service();
    assert(!note_all_off_requested); // A readiness pause is not a disconnect.
    ++model::epoch; service();
    assert(note_all_off_requested);
    int32_t stop_block[48];
    render_audio_samples(stop_block, 48);
    assert(!is_voice_active(0) && !note_all_off_requested);
    model::connected = true; ++model::epoch; service();
    assert(!note_all_off_requested);
    optional_services();
    for (unsigned arrivals : {1u, 100u, 10000u}) {
        publish_pending_voice_stream(0, reserve_voice_stream(0, 0, 9));
        int8_t tail = 1;
        assert(write_stream_body_samples(0, 9, &tail, 1) == 1);
        assert(activate_pending_voice_stream(0) == 0);
        tail_arrivals = arrivals;
        tail_peeks = 0;
        (void)render_voice_body_underrun(0, PHASE_ONE);
        assert(tail_peeks == 1); // Work depends on entry data, not new arrivals.
        assert(get_current_stream_buffered_samples(0) == 1);
        int8_t next = 0;
        assert(peek_stream_sample(0, 0, &next) == 0 && next == 2);
        tail_arrivals = 0;
        release_voice_stream(0);
        clear_voice_playhead(0);
    }
    note_hold_miss = 0;
    // Observe promotion before the renderer publishes its next voice snapshot.
    publish_pending_voice_stream(0, reserve_voice_stream(0, 0, 7));
    rs_tx.clear(); command("c:vq\r");
    assert(rs_tx.size() == 61 && (rs_tx[5] & 1) && rs_tx[12] == 7);
    assert(activate_pending_voice_stream(0) == 0);
    assert(!is_voice_active(0)); // deliberately stale snapshot
    rs_tx.clear(); command("c:vq\r");
    assert(rs_tx.size() == 61 && (rs_tx[4] & 1) && !(rs_tx[5] & 1));
    assert(rs_tx[12] == 7 && rs_tx[13] == 7);
    note_active[0] = 1;
    publish_voice_status();
    release_voice_stream(0);
    assert(is_voice_active(0)); // old renderer snapshot, retired stream session
    rs_tx.clear(); command("c:vq\r");
    assert(rs_tx.size() == 61 && !(rs_tx[4] & 1) && rs_tx[13] == 255);
    // An active voice must not dispatch a replacement handler before BODY is
    // prepared: user scripts are allowed to start the new note immediately.
    script_mask.store(1);
    post_voice_note_on(0, 60, 127, reserve_voice_stream(0, 0, 8));
    publish_prepared_voice_command(0);
    process_pending_voice_command(0);
    assert(note_command[0].load() != 0 && note_next_pending[0] == 0);
    assert(note_active[0] == 1);
    note_command[0].store(0);
    script_mask.store(0);
    release_voice_stream(0);
    note_active[0] = 0;
    publish_voice_status();

    // Pipelined silent notes use the existing RX buffer while audio owns roles.
    // No BODY is supplied, so this fixture never dispatches a VM handler.
    s_runtime.active_mask = 1;
    script_mask.store(1);
    rs_tx.clear();
    command("c:n0 on 0 60 127 @20\rc:n0 on 0 62 127 @21\rc:vq\r");
    assert(has_prepared_voice_command(0) && !rs_rx.empty());
    assert(!has_pending_voice_stream(0) && rs_tx.empty());
    assert(begin_voice_script_upload(0) == -2);
    assert(queue_streamed_sample_note(0, 0, 64, 127, 22) == -3);
    assert(queue_voice_note_off(0) == -3);
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(get_target_stream_session(0) == 20 && !has_prepared_voice_command(0));
    process_rs485_commands();
    assert(has_prepared_voice_command(0) && rs_rx.empty());
    // Queries remain serviceable: they report the last published session.
    assert(rs_tx.size() == 61 && rs_tx[12] == 20);
    assert(get_target_stream_session(0) == 20);
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(get_target_stream_session(0) == 21 && !has_prepared_voice_command(0));
    rs_tx.clear(); command("c:vq\r");
    assert(rs_rx.empty() && rs_tx.size() == 61 && rs_tx[12] == 21);
    // Hard clear must retire a reserved queue too, without losing ownership.
    assert(queue_streamed_sample_note(0, 0, 64, 127, 22) == 0);
    request_stop_all_voices();
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(!has_prepared_voice_command(0) && !has_pending_voice_stream(0));
    assert(get_stream_free_samples(0) == STREAM_RING_SAMPLES);
    // A second reservation can arrive after the boundary's first publication
    // pass. Clear must consume it too, not lose its storage with the command.
    assert(queue_streamed_sample_note(0, 0, 64, 127, 22) == 0);
    inject_clear_after_publication = true;
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(!has_prepared_voice_command(0) && !has_pending_voice_stream(0));
    // Exercise all three existing queue roles to expose any leaked reservation.
    assert(queue_streamed_sample_note(0, 0, 64, 127, 24) == 0);
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(activate_pending_voice_stream(0) == 0);
    assert(queue_streamed_sample_note(0, 0, 64, 127, 25) == 0);
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(queue_streamed_sample_note(0, 0, 64, 127, 26) == 0);
    request_stop_all_voices();
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    assert(get_stream_free_samples(0) == STREAM_RING_SAMPLES);
    // Different voices share one boundary; there is no global note queue.
    s_runtime.active_mask = 255;
    script_mask.store(255);
    for (unsigned v = 0; v < NOTE_BANK_VOICES; ++v) {
        char line[64];
        snprintf(line, sizeof line, "c:n%u on 0 60 127 @30\r", v);
        command(line);
        assert(has_prepared_voice_command(v));
    }
    assert(rs_rx.empty());
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    for (unsigned v = 0; v < NOTE_BANK_VOICES; ++v) {
        assert(!has_prepared_voice_command(v));
        assert(get_target_stream_session(v) == 30);
    }
    request_stop_all_voices();
    assert(begin_audio_runtime()); begin_voice_audio_block(); end_audio_runtime();
    for (unsigned v = 0; v < NOTE_BANK_VOICES; ++v)
        assert(get_stream_free_samples(v) == STREAM_RING_SAMPLES);
    s_runtime.active_mask = 0;
    script_mask.store(0);
    rs_tx.clear();
#endif
    // Byte-at-a-time input and combined frames yield the same ordered replies.
    for(uint32_t limit : {1u,7u,64u,1024u}) {
        read_limit=limit; hello();
        frame(USB_MSG_PROBE,{1,2,3}); frame(USB_MSG_PROBE);
        service(); assert(usb_rx.empty());
        assert(usb_tx.size()==18 && usb_tx[2]==USB_MSG_PROBE);
        assert(read_usb_uint32_le(usb_tx.data()+6)==3);
        assert(read_usb_uint32_le(usb_tx.data()+10)==1);
    }
    // An unqueued reply must survive backpressure without consuming next frame.
    hello(); tx_capacity=0;
    frame(USB_MSG_PROBE); frame(USB_MSG_PROBE,{9,8});
    service(); assert(usb_tx.empty() && usb_rx.size()==7);
    tx_capacity=2048; service(); assert(usb_tx.size()==18 && usb_rx.empty());
    // Queued data is copied; a rejected write accepts no prefix.
    usb_tx.clear(); uint8_t bytes[]={1,2,3};
    assert(write_usb_bytes(bytes,3)); bytes[0]=99; assert(usb_tx[0]==1);
    tx_capacity=4; assert(!write_usb_bytes(bytes,3)); assert(usb_tx.size()==3);
    tx_capacity=2048;
    // A new generation invalidates both a partial frame and a pending reply.
    hello(); usb_rx={USB_MSG_UPLOAD_BEGIN,0}; service();
    reconnect(); service(); frame(USB_MSG_HELLO,{USB_STREAM_VERSION}); service();
    assert(usb_tx.size()==19 && !faulted);
    usb_tx.clear(); tx_capacity=0; frame(USB_MSG_PROBE); service();
    reconnect(); tx_capacity=2048; service(); assert(usb_tx.empty());
    // Disconnect/readiness pause doesn't synthesize protocol input.
    connected=false; service(); assert(usb_tx.empty()); connected=true;
    hello();
    // Upload interruption and explicit abort do not publish incomplete data.
    frame(USB_MSG_UPLOAD_BEGIN,{USB_UPLOAD_ATTACK,4,0,0,0},7); service();
    assert(is_sample_upload_active());
    frame(USB_MSG_UPLOAD_DATA,{0,0,0,0,12,13},7); service();
    reconnect(); service(); assert(!is_sample_upload_active());
    assert(get_sample_length(7)==0);
    hello(); frame(USB_MSG_UPLOAD_BEGIN,{USB_UPLOAD_ATTACK,4,0,0,0},7); service();
    frame(USB_MSG_UPLOAD_ABORT); service(); assert(!is_sample_upload_active());
#ifndef LEGACY_API
    // Default policy retains partial frames and uploads across long pauses.
    hello(); usb_rx={USB_MSG_PROBE,0}; service(); time_ms+=60000; service();
    assert(!faulted);
    hello(); frame(USB_MSG_UPLOAD_BEGIN,{USB_UPLOAD_ATTACK,4,0,0,0},7); service();
    time_ms+=60000; service(); assert(is_sample_upload_active());
    reconnect(); service();
    TransportTimeouts timeouts{1000, 5000, 250, 5};
    set_transport_timeouts(&timeouts);
#endif
    // Explicitly configured partial-frame and upload deadlines.
    hello(); usb_rx={USB_MSG_PROBE,0}; service(); time_ms+=1001; service();
    assert(faulted);
    hello(); usb_rx={255,0,0,0,0}; service(); assert(faulted);
    // An upload that makes no progress expires by its existing deadline.
    hello(); frame(USB_MSG_UPLOAD_BEGIN,{USB_UPLOAD_ATTACK,4,0,0,0},7); service();
    time_ms+=5001; service(); assert(!is_sample_upload_active());
    // BODY stays pending until the selected stream has capacity.
    hello(); publish_pending_voice_stream(0, reserve_voice_stream(0,0,1));
    std::vector<uint8_t> body(1024,32);
    for(unsigned i=0;i<3;++i) { frame(USB_MSG_BODY,body,0,1); service(); }
    uint32_t received=get_usb_received_body_count();
    frame(USB_MSG_BODY,body,0,1); frame(USB_MSG_PROBE);
    service(); assert(get_usb_received_body_count()==received && usb_rx.size()==5);
    discard_pending_voice_stream(0); service(); assert(usb_rx.empty() && !faulted);
#ifndef LEGACY_API
    // Pending physical transmission services USB, preserves byte order, and
    // doesn't report completion merely because the bytes were accepted.
    hello(); rs_tx.clear(); frame(USB_MSG_PROBE,{5,6});
    unsigned serviced=usb_services; tx_polls=4;
    const uint8_t reply[]={4,3,2,1};
    assert(write_rs485_bytes(reply,4,100)==0);
    assert(!bus_owned);
    assert(rs_tx==std::vector<uint8_t>(reply,reply+4));
    assert(usb_services>serviced);
    tx_stalled=true; unsigned aborted=aborts;
    command("c:vm\r"); assert(aborts==aborted+1 && !bus_owned);
    tx_stalled=false; command("c:vm\r");
    assert(start_rs485_transmit(reply,4)==0);
    assert(write_rs485_bytes(reply,4,100)==-1 && !bus_owned);
    assert(start_rs485_transmit(reply,4)==-1); abort_rs485_transmit();
    // Wrap-safe timeout; receive reset preserves lifetime drop accounting.
    time_ms=0xfffffffdu; tx_stalled=true;
    assert(write_rs485_bytes(reply,4,5)==-1); tx_stalled=false;
    assert(!bus_owned);
    bus_owned=true; assert(!wait_for_rs485_bus(5)); bus_owned=false;
    assert(wait_for_rs485_bus(5));
    set_transport_timeouts(nullptr);
    tx_polls=100; time_ms=0xfffffffdu;
    assert(write_rs485_bytes(reply,4,0)==0 && !bus_owned);
    drops=9; command("c:reset\rc:vm\r"); assert(rs_rx.empty() && drops==9);
#endif
    return 0;
}
