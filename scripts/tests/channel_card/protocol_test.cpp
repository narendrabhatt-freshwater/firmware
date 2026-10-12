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
#include "usb.h"
#include "stream.h"
#include "samples.h"
#ifndef LEGACY_API
#include "transport.h"
#endif
#include <assert.h>
#include <string.h>
#include <vector>

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
    update_channel_leds();
#endif
    assert(!bus_owned);
}
int main()
{
    init_usb_protocol(); init_channel_card();
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
    // Existing partial-frame timeout and malformed-header fault policies.
    hello(); usb_rx={USB_MSG_PROBE,0}; service(); time_ms+=1001; service();
    assert(faulted);
    hello(); usb_rx={255,0,0,0,0}; service(); assert(faulted);
    // An upload that makes no progress expires by its existing deadline.
    hello(); frame(USB_MSG_UPLOAD_BEGIN,{USB_UPLOAD_ATTACK,4,0,0,0},7); service();
    time_ms+=5001; service(); assert(!is_sample_upload_active());
    // BODY stays pending until the selected stream has capacity.
    hello(); prepare_pending_voice_stream(0,0,1);
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
    assert(rs_tx==std::vector<uint8_t>(reply,reply+4));
    assert(usb_services>serviced);
    tx_stalled=true; unsigned aborted=aborts;
    command("c:vm\r"); assert(aborts==aborted+1 && !bus_owned);
    tx_stalled=false; command("c:vm\r");
    assert(start_rs485_transmit(reply,4)==0);
    assert(start_rs485_transmit(reply,4)==-1); abort_rs485_transmit();
    // Wrap-safe timeout; receive reset preserves lifetime drop accounting.
    time_ms=0xfffffffdu; tx_stalled=true;
    assert(write_rs485_bytes(reply,4,5)==-1); tx_stalled=false;
    drops=9; command("c:reset\rc:vm\r"); assert(rs_rx.empty() && drops==9);
#endif
    return 0;
}
