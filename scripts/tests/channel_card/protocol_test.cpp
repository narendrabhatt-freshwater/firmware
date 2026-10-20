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
#include "application.h"
#include "transport.h"
#include "serial_buffer.h"
#include "usb.h"
#include "stream.h"
#include "samples.h"
#include "transport.h"
#include "platform.h"
#include <assert.h>
#include <cstring>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

using namespace model;
/* ---- append a BODY frame or one complete upload to test input ------------ */

static void frame(uint8_t type, std::vector<uint8_t> data = {}, uint8_t target = 0, uint8_t session = 0)
{
    if (type == USB_MSG_UPLOAD)
    {
        assert(data.size() >= 5);
        usb_rx.insert(usb_rx.end(), {type, data[0], target});
        usb_rx.insert(usb_rx.end(), data.begin() + 1, data.end());
        return;
    }
    uint8_t header[] = {type, target, session, (uint8_t)data.size(), (uint8_t)(data.size() >> 8)};
    usb_rx.insert(usb_rx.end(), header, header + 5);
    usb_rx.insert(usb_rx.end(), data.begin(), data.end());
}

/* ---- run bounded receive and transmit passes ----------------------------- */

static void service(unsigned count = 64)
{
    while (count--)
    {
        service_usb();
    }
}

/* ---- start a fresh binary connection without a handshake ----------------- */

static void connect_usb()
{
    reconnect();
    service();
    assert(usb_tx.empty() && !usb_serial_buffer.faulted);
}

static void command(const char *text)
{
    rs_rx.insert(rs_rx.end(),text,text+strlen(text));
    unsigned passes = 0;
    do {
        service_rs485();
        assert(++passes < 1000);
    } while (!rs_rx.empty() || rs485_serial_buffer.input_size || has_pending_rs485_reply());
}
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
    uint32_t saved = disable_interrupts_and_save_state();
    restore_interrupt_state(saved);
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
int main()
{
    init_usb_device(); init_application();
    while (has_pending_rs485_reply()) service_rs485();
    optional_services();
    for (uint32_t limit : {1u, 7u, 64u, 1024u})
    {
        read_limit = limit;
        connect_usb();
        frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 10, 20}, 7);
        frame(USB_MSG_UPLOAD, {2, 2, 0, 0, 0, 30, 40}, 6);
        service();
        assert(usb_rx.empty());
        assert(usb_tx == std::vector<uint8_t>({7, 2, 0, 6, 2, 0}));
    }
    connect_usb();
    frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 10, 20}, 7);
    frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 30, 40}, 6);
    std::memcpy(usb_serial_buffer.input, usb_rx.data(), usb_rx.size());
    usb_serial_buffer.input_size = usb_rx.size();
    usb_rx.clear();
    size_t pending = usb_serial_buffer.input_size;
    usb_serial_buffer.output_size = sizeof usb_serial_buffer.output;
    process_usb_messages();
    assert(usb_serial_buffer.input_size == pending);
    usb_serial_buffer.output_size = 0;
    service();
    assert(usb_tx == std::vector<uint8_t>({7, 2, 0, 6, 2, 0}));

    uint8_t bytes[] = {1, 2, 3};
    assert(queue_serial_output(usb_serial_buffer, bytes, 3));
    bytes[0] = 99;
    assert(usb_serial_buffer.output[0] == 1);
    usb_serial_buffer.output_size = sizeof usb_serial_buffer.output - 2;
    assert(!queue_serial_output(usb_serial_buffer, bytes, 3));
    assert(usb_serial_buffer.output_size == sizeof usb_serial_buffer.output - 2);
    usb_serial_buffer.output_size = 0;

    connect_usb();
    usb_rx = {USB_MSG_UPLOAD, 1};
    service();
    connect_usb();
    frame(USB_MSG_UPLOAD, {1, 4, 0, 0, 0, 12, 13}, 8);
    service();
    assert(is_sample_upload_active() && usb_tx.empty());
    connect_usb();
    assert(!is_sample_upload_active() && get_sample_length(8) == 0);

    usb_rx = {USB_MSG_BODY, 0};
    service();
    time_ms += 1001;
    service();
    assert(usb_serial_buffer.faulted && usb_tx.empty());
    connect_usb();
    usb_rx = {255};
    service();
    assert(usb_serial_buffer.faulted && usb_tx.empty());
    connect_usb();
    frame(USB_MSG_UPLOAD, {1, 4, 0, 0, 0, 12, 13}, 8);
    service();
    time_ms += 5001;
    service();
    assert(!is_sample_upload_active() && usb_serial_buffer.faulted);
    assert(usb_tx.size() > 3 && usb_tx[0] == 8 && usb_tx[1] == 2 && usb_tx[2] == 1);

    connect_usb();
    frame(USB_MSG_BODY, {12}, 8, 1);
    service();
    assert(usb_serial_buffer.faulted && usb_tx.empty());
    connect_usb();
    prepare_pending_voice_stream(0, 0, 1);
    std::vector<uint8_t> body(1024, 32);
    for (unsigned i = 0; i < 3; ++i)
    {
        frame(USB_MSG_BODY, body, 0, 1);
        service();
    }
    uint32_t received = get_usb_received_body_count();
    frame(USB_MSG_BODY, body, 0, 1);
    frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 12, 13}, 7);
    service();
    assert(get_usb_received_body_count() == received && usb_tx.empty());
    assert(usb_rx.size() + usb_serial_buffer.input_size == 9);
    discard_pending_voice_stream(0);
    service();
    assert(usb_rx.empty() && !usb_serial_buffer.input_size && !usb_serial_buffer.faulted);
    assert(usb_tx == std::vector<uint8_t>({7, 2, 0}));
    // Replies stay pending until the platform drains the shared output.
    connect_usb(); rs_tx.clear();
    uint8_t reply[]={4,3,2,1};
    assert(queue_rs485_reply(reply,4,nullptr)==0);
    reply[0]=99;
    process_rs485_replies();
    assert(rs485_serial_buffer.output_size==4);
    assert(rs485_serial_buffer.output[0]==4);
    process_rs485_replies();
    assert(has_pending_rs485_reply());
    frame(USB_MSG_UPLOAD, {1, 2, 0, 0, 0, 12, 13}, 7); service(); assert(!usb_tx.empty());
    service_rs485(); service_rs485();
    assert(!has_pending_rs485_reply());
    assert(rs_tx==std::vector<uint8_t>({4,3,2,1}));

    // The driver reports a failed transmission without silently accepting it.
    static uint8_t completed = 2;
    assert(queue_rs485_reply(reply,4,[](uint8_t ok) { completed=ok; })==0);
    process_rs485_replies();
    rs485_serial_buffer.transmit_failed = true;
    rs485_serial_buffer.output_size = 0;
    process_rs485_replies();
    assert(completed==0 && !has_pending_rs485_reply());

    // Capacity is the largest existing multi-line response, not an unbounded backlog.
    for (unsigned i=0;i<RS485_REPLY_SLOTS;++i) assert(queue_rs485_reply(reply,4,nullptr)==0);
    assert(queue_rs485_reply(reply,4,nullptr)==-1);
    while (has_pending_rs485_reply()) service_rs485();
    rs_tx.clear(); command("c:vm mem\r");
    assert(std::count(rs_tx.begin(),rs_tx.end(),'\n')==9);
    for (unsigned i=0;i<8;++i) {
        std::string line="[C] ok:vm memv "+std::to_string(i)+" ";
        assert(std::string(rs_tx.begin(),rs_tx.end()).find(line)!=std::string::npos);
    }
    rs485_serial_buffer.dropped=9; command("c:reset\rc:vm\r");
    assert(rs_rx.empty() && rs485_serial_buffer.dropped==9);
    return 0;
}
