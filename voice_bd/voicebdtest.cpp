#if defined(VOICEBD_TRANSPORT_TEST)
// Exercise the production host scheduler against the actual firmware ring code.
#include "voicebd.cpp"
#include "../channel_card/USB_APP/usb_protocol.h"
#include "../channel_card/Core/Inc/audio/stream_ring.h"
#include <stdexcept>
#include <sys/socket.h>
using namespace voice_board_detail;
static void require(bool condition,const char* what) { if (!condition) throw std::runtime_error(what); }
static card_status snapshot(uint8_t sequence) {
    card_status s{}; s.status_sequence=sequence; s.buffer_capacity=STREAM_RING_SAMPLES;
    s.last_usb_sequence=StreamRing_LastBodySequence();
    for (uint8_t v=0;v<8;++v) {
        s.free_samples[v]=static_cast<uint16_t>(StreamRing_FreeLevel(v));
        s.session_id[v]=StreamRing_TargetSession(v);
        if (StreamRing_HasPending(v)) s.pending_voice_mask|=static_cast<uint8_t>(1u<<v);
        else if (s.session_id[v]!=255) s.active_voice_mask|=static_cast<uint8_t>(1u<<v);
        s.remaining_us[v]=StreamRing_CurrentFill(v)*1000000u/48000u;
        s.refill_samples_5ms[v]=240;
    }
    return s;
}
static void arm(stream_state& host,uint8_t voice,uint8_t session) {
    auto& sample=host.samples[voice]; sample.pcm_size=20000; sample.loaded=true;
    sample.pcm=std::make_unique<int16_t[]>(sample.pcm_size);
    for (size_t i=0;i<sample.pcm_size;++i) sample.pcm[i]=static_cast<int16_t>(i*256);
    auto& v=host.voices[voice]; v.active=true; v.pending=true; v.session_id=session;
    v.sample_id=voice; v.waiting_for_first_packet=true; v.initial_samples_left=kPrimeSamples;
    StreamRing_ArmPending(voice,voice,session);
}
static void framing_test() {
    static_assert(kUsbHeaderBytes==USB_STREAM_HEADER_BYTES && kUsbPayloadMax==USB_STREAM_PAYLOAD_MAX);
    static_assert(unsigned(kBody)==USB_MSG_BODY && unsigned(kReply)==USB_MSG_REPLY);
    std::array<uint8_t,1032> frame{};
    frame[0]=kBody; frame[1]=7; frame[2]=254; frame[3]=1;
    write16(frame.data()+4,1024); write16(frame.data()+6,65535);
    for (size_t i=8;i<frame.size();++i) frame[i]=static_cast<uint8_t>(i);
    // Every possible two-read split, including inside length and sequence.
    for (size_t split=0;split<=frame.size();++split) {
        USB_Parser p{}; USB_ParserReset(&p); int complete=0;
        for (size_t start=0,end=split,pass=0;pass<2;++pass,start=split,end=frame.size())
            for (size_t i=start;i<end;++i) {
                int rc=USB_ParserByte(&p,frame[i]); require(rc>=0,"fragment rejected"); complete+=rc;
            }
        require(complete==1 && std::equal(frame.begin(),frame.end(),p.bytes),"fragment corruption");
    }
    USB_Parser p{}; USB_ParserReset(&p); int complete=0;
    for (unsigned block=0;block<3;++block) for (auto byte:frame) {
        int rc=USB_ParserByte(&p,byte);
        if (rc==1) { ++complete; USB_ParserReset(&p); }
    }
    require(complete==3,"coalesced frames");
    frame[4]=1; frame[5]=4; USB_ParserReset(&p);
    int rc=0; for (unsigned i=0;i<8;++i) rc=USB_ParserByte(&p,frame[i]);
    require(rc==-1,"oversized payload accepted");
}
static void credit_test() {
    StreamRing_Init(); stream_state h; arm(h,0,1);
    auto now=std::chrono::steady_clock::now(); auto s=snapshot(1); h.apply_status(s,now);
    uint8_t frame[1032]; unsigned sent=0;
    while (auto n=h.make_block(frame)) { sent+=static_cast<unsigned>(n-8); }
    require(sent==4080,"exact credit not enforced");
    s.status_sequence=2; h.apply_status(s,now); // Nothing delivered: do not grant it again.
    require(h.make_block(frame)==0,"in-flight credit double spent");
    // New note shares the physical ring: old-session traffic is still charged.
    h.voices[0].session_id=2; StreamRing_ArmPending(0,0,2);
    s=snapshot(3); h.apply_status(s,now);
    require(h.make_block(frame)==0,"old-session outstanding bytes forgotten");
    // Card processes/discards all old blocks, allowing a fresh exact snapshot.
    s.last_usb_sequence=static_cast<uint16_t>(h.next_sequence-1); s.status_sequence=4;
    h.apply_status(s,now); require(h.make_block(frame)>0,"ack did not release credit");
    // Idle connection emits no padding.
    for (auto& v:h.voices) v.active=false;
    require(h.make_block(frame)==0,"idle BODY traffic");
}
static void byte_window_test() {
    StreamRing_Init(); stream_state h; for(uint8_t v=0;v<8;++v)arm(h,v,1);
    auto now=std::chrono::steady_clock::now(); auto status=snapshot(1);
    for(auto& credit:status.free_samples)credit=500;
    h.apply_status(status,now); uint8_t frame[1032];unsigned bytes=0,blocks=0;
    while(auto n=h.make_block(frame)) {bytes+=static_cast<unsigned>(n);++blocks;}
    require(blocks==8,"small-block first window");
    // A fresh snapshot can grant more real space without ACKing earlier blocks.
    // Block count must not stop submission while the byte window has room.
    status.status_sequence=2;for(auto& credit:status.free_samples)credit=1000;
    h.apply_status(status,now);
    while(auto n=h.make_block(frame)) {bytes+=static_cast<unsigned>(n);++blocks;}
    require(blocks>8 && bytes<=kPendingWireBytes,"small blocks exhausted packet-count window");
    status.status_sequence=3;for(auto& credit:status.free_samples)credit=4080;
    h.apply_status(status,now);
    while(auto n=h.make_block(frame))bytes+=static_cast<unsigned>(n);
    require(bytes==kPendingWireBytes,"wire-byte bound exceeded or underused");
}
static void forecast_test() {
    StreamRing_Init();stream_state h;arm(h,0,1);
    h.voices[0].initial_samples_left=0;h.voices[0].waiting_for_first_packet=false;
    auto now=std::chrono::steady_clock::now();auto status=snapshot(1);
    status.active_voice_mask=1;status.pending_voice_mask=0;
    status.free_samples[0]=100;status.refill_samples_5ms[0]=240;status.remaining_us[0]=80000;
    h.apply_status(status,now,now);uint8_t frame[1032];
    require(h.make_block(frame,now)==0,"small refill did not retain safety reserve");
    require(h.make_block(frame,now+std::chrono::milliseconds(5))==252,"five-ms look-ahead was lost");
    require(h.make_block(frame,now+std::chrono::milliseconds(50))==0,"forecast continued beyond fresh status horizon");
    status.status_sequence=2;status.free_samples[0]=340; // The 244 sent bytes remain unacknowledged.
    h.apply_status(status,now+std::chrono::milliseconds(50),now+std::chrono::milliseconds(50));
    require(h.make_block(frame,now+std::chrono::milliseconds(50))==0,"forecast forgot unacknowledged data");
}
static void ring_test() {
    StreamRing_Init(); StreamRing_ArmPending(0,0,254);
    int8_t bytes[1024]; for (unsigned i=0;i<1024;++i) bytes[i]=static_cast<int8_t>(i);
    require(StreamRing_WriteBody(0,254,1,65535,bytes,998)==1,"prime failed");
    require(StreamRing_StartNote(0)==0,"start failed");
    require(StreamRing_WriteBody(0,254,0,0,bytes,1024)==1,"sequence wrap failed");
    require(StreamRing_LastBodySequence()==0,"wrapped ack");
    StreamRing_Advance(0,1900);
    for (unsigned i=0;i<3;++i) require(StreamRing_WriteBody(0,254,0,i+1,bytes,1024)==1,"ring wrap write");
    unsigned before=StreamRing_CurrentFill(0);
    require(StreamRing_WriteBody(0,254,0,4,bytes,1024)==-1,"overflow accepted");
    require(StreamRing_CurrentFill(0)==before,"overflow changed ring");
    StreamRing_ArmPending(0,0,0);
    require(StreamRing_WriteBody(0,254,0,5,bytes,1)==0,"retired session accepted");
    require(StreamRing_LastBodySequence()==5,"stale data not acknowledged");
    // Incomplete reservations cannot publish samples.
    StreamRing_ResetAll(); StreamRing_ArmPending(1,1,3); StreamRing_Write_t w;
    require(StreamRing_WriteBegin(1,3,1,1,20,&w)==0,"reservation failed");
    uint32_t n; auto dst=StreamRing_WriteSpan(&w,&n); memcpy(dst,bytes,10);
    require(StreamRing_WriteAdvance(&w,10)==0,"advance failed");
    require(StreamRing_WriteCommit(&w)==0 && StreamRing_PendingFill(1)==0,"partial block published");
    StreamRing_WriteAbort(&w);
    // Audio boundary promotion during the next copy must not lose samples.
    StreamRing_ResetAll(); StreamRing_ArmPending(0,0,1);
    require(StreamRing_WriteBody(0,1,1,1,bytes,998)==1,"promotion prime");
    require(StreamRing_WriteBegin(0,1,0,0,20,&w)==0,"promotion reservation");
    dst=StreamRing_WriteSpan(&w,&n); memcpy(dst,bytes,20);
    require(StreamRing_WriteAdvance(&w,20)==0,"promotion advance");
    require(StreamRing_StartNote(0)==0,"promotion");
    require(StreamRing_WriteCommit(&w)==20 && StreamRing_CurrentFill(0)==1018,"promotion lost BODY");
}
static void streaming_test(unsigned consume=48, unsigned status_interval=8) {
    StreamRing_Init(); stream_state h; for (uint8_t v=0;v<8;++v) arm(h,v,1);
    auto now=std::chrono::steady_clock::now(); uint8_t status_seq=0;
    h.apply_status(snapshot(++status_seq),now);
    std::array<bool,8> started{}; uint8_t frame[1032];
    // One maximum BODY block per millisecond. Include repeated 8 ms status gaps.
    for (unsigned ms=0;ms<30000;++ms) {
        if (ms%status_interval==0) {
            auto s=snapshot(++status_seq);
            for (unsigned v=0;v<8;++v) {
                s.remaining_us[v]=StreamRing_CurrentFill(v)*1000u/consume;
                s.refill_samples_5ms[v]=static_cast<uint16_t>(consume*5);
            }
            h.apply_status(s,now+std::chrono::milliseconds(ms),now+std::chrono::milliseconds(ms));
        }
        size_t n=h.make_block(frame,now+std::chrono::milliseconds(ms));
        if (n) {
            unsigned voice=frame[1];
            require(StreamRing_WriteBody(frame[1],frame[2],frame[3],read16(frame+6),
                reinterpret_cast<int8_t*>(frame+8),static_cast<uint16_t>(n-8))==1,"valid stream block rejected");
            if (!started[voice] && StreamRing_PendingFill(voice)>=998) {
                require(StreamRing_StartNote(static_cast<uint8_t>(voice))==0,"simulated startup"); started[voice]=true;
            }
        }
        for (uint8_t v=0;v<8;++v) if (started[v]) {
            if (StreamRing_CurrentFill(v)<consume) throw std::runtime_error(
                "simulated underrun at ms="+std::to_string(ms)+" voice="+std::to_string(v)+
                " fill="+std::to_string(StreamRing_CurrentFill(v))+" outstanding="+std::to_string(h.pending_packet_count));
            StreamRing_Advance(v,consume);
        }
    }
    for (bool active:started) require(active,"voice starved during startup");
}
static void worker_test(bool bad_reply) {
    int sockets[2]; require(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0,"socket pair");
    std::atomic<bool> stop{false}, peer_ok{true}; std::atomic<unsigned> body_count{0}, upload_count{0}, body_bytes{0}, starts{0};
    std::thread peer([&] {
        USB_Parser parser{}; USB_ParserReset(&parser);
        while(!stop) {
            pollfd fd{sockets[1],POLLIN,0};
            int ready=::poll(&fd,1,50); if(ready<=0)continue;
            uint8_t b; if(::read(sockets[1],&b,1)!=1)break;
            int rc=USB_ParserByte(&parser,b);
            if(rc<0) { peer_ok=false; break; }
            if(rc!=1)continue;
            auto h=parser.bytes;
            if(h[0]==kBody) { ++body_count; body_bytes+=8+read16(h+4); starts+=(h[3]&1); USB_ParserReset(&parser); continue; }
            if(h[0]==kUploadBegin || h[0]==kUploadData)++upload_count;
            uint8_t reply[20]={kReply,h[1],h[2],h[0],1,0,h[6],h[7],0};
            unsigned size=9;
            if(h[0]==kHello) {
                reply[4]=12;reply[9]=1;reply[10]=8;reply[11]=1;
                write32(reply+12,48000);write16(reply+16,1024);write16(reply+18,998);size=20;
            }
            if(bad_reply)reply[6]^=0x40; // Mismatched request must never complete successfully.
            for(unsigned i=0;i<size;++i) if(::write(sockets[1],reply+i,1)!=1)peer_ok=false;
            USB_ParserReset(&parser);
        }
    });
    stream_state host; StreamRing_Init(); usb_link link;
    auto opened=link.open_test_fd(sockets[0],host);
    if(!bad_reply && opened) {
        auto now=std::chrono::steady_clock::now();
        for(uint8_t v=0;v<8;++v)arm(host,v,1);
        host.apply_status(snapshot(1),now);link.enable_streaming();
        uint8_t sample[512]={};
        auto uploaded=link.upload(1,0,sample,sizeof sample);
        require(uploaded.ok(),"upload while BODY is running");
        require(starts==8 && body_count>8 && body_bytes<=kPendingWireBytes && upload_count==2,"BODY byte window / upload interleaving");
    }
    link.close();stop=true;::shutdown(sockets[1],SHUT_RDWR);peer.join();::close(sockets[1]);
    require(peer_ok,"peer framing failed under partial writes");
    require(bad_reply ? !opened.ok() : opened.ok(),"request reply validation");
}
static void benchmark_test(bool corrupt) {
    int sockets[2]; require(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0,"benchmark socket pair");
    std::atomic<bool> stop{false},peer_ok{true};
    std::thread peer([&] {
        USB_Parser parser{}; USB_ParserReset(&parser);
        uint32_t bytes=0,blocks=0,hash=2166136261u;
        while(!stop) {
            pollfd fd{sockets[1],POLLIN,0}; if(::poll(&fd,1,50)<=0)continue;
            uint8_t buffer[256]; auto n=::read(sockets[1],buffer,sizeof buffer); if(n<=0)break;
            for(int i=0;i<n;++i) {
                int rc=USB_ParserByte(&parser,buffer[i]);
                if(rc<0) { peer_ok=false; return; }
                if(!rc)continue;
                auto h=parser.bytes; unsigned length=read16(h+4);
                uint8_t response[21]={kReply,0,0,h[0],0,0,h[6],h[7],0};
                unsigned size=0;
                if(h[0]==kHello) {
                    response[4]=12;response[9]=1;response[10]=8;response[11]=1;
                    write32(response+12,48000);write16(response+16,1024);write16(response+18,998);size=20;
                } else if(h[0]==kProbe && length) {
                    bytes+=length;++blocks;
                    for(unsigned j=0;j<length;++j)hash=(hash^h[8+j])*16777619u;
                } else if(h[0]==kProbe) {
                    response[4]=13;write32(response+9,bytes);write32(response+13,blocks);
                    write32(response+17,hash^(corrupt ? 1u : 0u));size=21;
                } else { peer_ok=false; return; }
                for(unsigned j=0;j<size;++j) if(::write(sockets[1],response+j,1)!=1)peer_ok=false;
                USB_ParserReset(&parser);
            }
        }
    });
    serial_port port; port.adopt_test_fd(sockets[0]);
    // Zero seconds performs one throughput batch plus the latency observations.
    auto result=usb_benchmark(port,0);
    stop=true;::shutdown(sockets[1],SHUT_RDWR);peer.join();::close(sockets[1]);
    require(peer_ok,"benchmark peer framing");
    require(corrupt ? !result.ok() : result.ok(),"benchmark verifies byte counts and checksum");
}
int main() try {
    framing_test(); credit_test(); byte_window_test(); forecast_test(); ring_test(); streaming_test(); streaming_test(120,5); worker_test(false); worker_test(true); benchmark_test(false); benchmark_test(true);
    std::cout << "CDC framing, credit, sessions, worker I/O and eight-voice simulations (384/960 kB/s) passed\n";
    return 0;
} catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
#elif defined(VOICEBD_HARDWARE_TEST)
// Opt-in hardware qualification; never registered as an automatic CTest.
#include "voicebd.cpp"
#include <stdexcept>
using namespace voice_board_detail;
static void checked(voice_board_result_t const& r) {
    if(!r) throw std::runtime_error(r.message);
}
static uint64_t counter(std::string const& line, std::string const& name) {
    auto at=line.find(" "+name+" ");
    if(at==std::string::npos) throw std::runtime_error("missing counter: "+name);
    return std::stoull(line.substr(at+name.size()+2));
}
static void clean_counters(std::string const& line) {
    for(auto name:{"drop","hold","full","bad","late","future"})
        if(counter(line,name)) throw std::runtime_error("hardware counter increased: "+std::string(name));
}
int main(int argc,char**argv) try {
    if(argc<5 || argc>8) {
        std::cerr<<"usage: voicebd_hardware_test RS485 USB BEC SECONDS [KEY=72] [steady|exercise] [REOPENS=0]\n";
        return 2;
    }
    unsigned seconds=static_cast<unsigned>(std::stoul(argv[4]));
    unsigned key=argc>5 ? static_cast<unsigned>(std::stoul(argv[5])) : 72;
    std::string mode=argc>6 ? argv[6] : "steady";
    unsigned reopens=argc>7 ? static_cast<unsigned>(std::stoul(argv[7])) : 0;
    if(!seconds || seconds>1800 || key>127 || reopens>100 || (mode!="steady" && mode!="exercise"))
        throw std::runtime_error("invalid test duration, key, mode or reopen count");
    voice_board_config_t config;
    config.rs485_port=argv[1];config.usb_port=argv[2];config.bec_file=argv[3];
    config.initial_attenuation_db=48;
    std::vector<int16_t> pcm(48000*120);
    for(size_t i=0;i<pcm.size();++i)
        pcm[i]=static_cast<int16_t>(4096*std::sin(i*6.283185307179586*261.625565/48000));
    device_context state;
    checked(open_device(&state,config));checked(load_sample(&state,0,pcm,261.625565));
    auto diagnostic=[&](std::string const& cmd) {
        device_context::control_turn turn(&state);
        auto r=state.rs485.command(cmd);checked(r);return r.message;
    };
    auto stats=[&] {
        auto line=diagnostic("usb");std::cout<<line<<std::endl;clean_counters(line);return line;
    };
    auto rs_before=diagnostic("rs485");
    (void)diagnostic("usb 0");
    auto chord=[&] {
        for(uint8_t v=0;v<8;++v)checked(note_on(&state,v,0,static_cast<uint8_t>(key),100));
    };
    chord();
    auto begin=std::chrono::steady_clock::now();
    auto end=begin+std::chrono::seconds(seconds);
    auto report=begin+std::chrono::seconds(10);
    auto retrigger=begin+std::chrono::seconds(30);
    auto action=begin+std::chrono::milliseconds(100);
    unsigned actions=0;bool script_rejected=false;
    while(std::chrono::steady_clock::now()<end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(!device_is_open(&state)) throw std::runtime_error("transport failed during playback");
        auto now=std::chrono::steady_clock::now();
        if(mode=="exercise" && now>=action) {
            uint8_t v=static_cast<uint8_t>(actions%8);
            checked(note_on(&state,v,0,static_cast<uint8_t>(actions%2 ? key : 60),100));
            if(actions%10==5)checked(note_off(&state,static_cast<uint8_t>((v+3)%8)));
            if(actions%20==10)checked(load_sample(&state,0,pcm,261.625565));
            if(!script_rejected && actions>=10) {
                if(load_script(&state,0,config.bec_file))
                    throw std::runtime_error("active script replacement was not rejected");
                if(!device_is_open(&state)) throw std::runtime_error("script rejection broke USB");
                script_rejected=true;
            }
            ++actions;action=now+std::chrono::milliseconds(100);
        }
        // Keep nonzero source data throughout long runs; include new sessions.
        if(now>=retrigger) {chord();retrigger=now+std::chrono::seconds(30);}
        if(now>=report) {
            std::cout<<"elapsed "<<std::chrono::duration_cast<std::chrono::seconds>(now-begin).count()<<" s ";
            (void)stats();report=now+std::chrono::seconds(10);
        }
    }
    (void)stats();checked(all_notes_off(&state));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto idle_before=stats();std::this_thread::sleep_for(std::chrono::seconds(2));auto idle_after=stats();
    if(counter(idle_before,"bytes")!=counter(idle_after,"bytes"))
        throw std::runtime_error("USB data continued while idle");
    for(unsigned i=0;i<reopens;++i) {
        checked(close_device(&state));checked(open_device(&state,config));
        checked(load_sample(&state,0,pcm,261.625565));chord();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(reopens) (void)stats();
    // Stop remains available through RS485 when the USB worker is unavailable.
    checked(note_on(&state,0,0,60,100));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    state.worker_running=false;state.control_changed.notify_all();state.worker.join();
    state.usb.close();checked(note_off(&state,0));checked(all_notes_off(&state));
    (void)stats();auto rs_after=diagnostic("rs485");std::cout<<rs_after<<std::endl;
    for(auto name:{"rxdrop","txfail","trunc"})
        if(counter(rs_before,name)!=counter(rs_after,name))
            throw std::runtime_error("RS485 counter increased: "+std::string(name));
    checked(close_device(&state));
    std::cout<<"Hardware counters, idle traffic and RS485 stop after USB close passed. "
             <<"This does not measure note-to-DAC latency.\n";
    return 0;
} catch(std::exception const& e) {std::cerr<<e.what()<<'\n';return 1;}

#else
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
#include <RtMidi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sysexits.h>
#include <fstream>
#include <filesystem>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
char const* exe_name;
char const version[] = "0.01";
static char const product[] = "172-XXXX";
char const name[] = "Channel card voice board test";
// Change to 2..8 for polyphony; 1 sends every press/release directly to voice 0.
constexpr unsigned num_voices = 8;
static_assert(num_voices >= 1 && num_voices <= voice_board_t::voice_count,
              "num_voices must be between 1 and 8");
constexpr uint32_t sample_rate_hz = 48000;
constexpr double sample_root_hz = 261.625565; // C4, MIDI key 60.

/* Nominal BODY demand; a custom card program can change the playback pitch. */
void print_key_demand(unsigned voice, unsigned key)
{
    static char const* const notes[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    double const hz = 440.0 * std::exp2((int(key) - 69) / 12.0);
    double const speed = std::clamp(hz / sample_root_hz, 1.0 / 16.0, 16.0);
    std::ostringstream line;
    line << "key=" << key << " (" << notes[key % 12] << int(key / 12) - 1
         << ") voice=" << voice << " required=" << std::fixed << std::setprecision(2)
         << sample_rate_hz / 1000.0 * speed << " samples/ms\n";
    std::cout << line.str() << std::flush;
}

/* ---- print usage --------------------------------------------------------- */

int usage(int status)
{
    std::ostream& out = status == EX_OK ? std::cout : std::cerr;
    out << "\nNAME\n\n"
        << "    " << exe_name << " - " << name
        << " (" << product << "). Version " << version << "\n"
        << "    (C) 2026 Freshwater Instruments\n"
        << "\nUSAGE\n\n"
        << "    " << exe_name << " [options] <sample.wav> [program.bec]\n"
        << "\nOPTIONS\n\n"
        << "    -h             Print this help and exit\n"
        << "    --usb-port PORT     Override the Channel CDC port\n"
        << "    --rs485-port PORT   Override the RS485 adapter port\n"
        << "    --usb-bench PORT [SECONDS]   USB-only throughput/integrity/timing check (default 5 s)\n"
        << "\nARGUMENTS\n\n"
        << "    sample.wav     48 kHz, 16-bit PCM WAV, mono or stereo\n"
        << "    program.bec    Firmware program (default: channel.bec in current directory)\n"
        << "\n    Uses MIDI input 0 and the device ports configured in voicebd.h.\n"
        << "    Tests loadScript on voices 0-7 after opening the board.\n"
        << "    MIDI voices: " << num_voices << " (1 = direct mono, 2..8 = polyphony).\n"
        << "    O: orchestra, S: sine, P: square, T: triangle, W: sawtooth, A: attack replacement test; R: compile/upload series2.be; Space: hard stop/reset; Ctrl+C exits.\n\n";
    return status;
}

/* ---- handle termination signals ----------------------------------------- */

volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

/* Read terminal keys immediately, restoring the terminal on normal exit/errors.
 * Keep ISIG enabled so Ctrl+C still goes through the termination handler. */
struct terminal_keys {
    termios saved{};
    bool changed = false;
    terminal_keys() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios mode = saved;
        mode.c_lflag &= ~(ICANON | ECHO);
        changed = tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
    }
    ~terminal_keys() {
        if (changed) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    int read_key() const {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        char key;
        return poll(&input, 1, 0) > 0 && (input.revents & POLLIN) &&
            read(STDIN_FILENO, &key, 1) == 1 ? key : -1;
    }
};

/* Assumes a valid PCM16 mono/stereo WAV; convert its rate to 48 kHz. */
std::vector<int16_t> read_wav(char const* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open WAV: " + std::string(path));
    file.exceptions(std::ios::failbit | std::ios::badbit);
    auto number = [&](unsigned n) {
        uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= uint32_t(uint8_t(file.get())) << (8 * i);
        return value;
    };
    file.ignore(12);
    unsigned channels = 0;
    uint32_t rate = sample_rate_hz;
    for (;;) {
        char tag[4];
        file.read(tag, 4);
        uint32_t const size = number(4);
        if (std::string(tag, 4) == "fmt ") {
            file.ignore(2);
            channels = number(2);
            rate = number(4);
            file.ignore(8);
            file.ignore(uint64_t(size) - 16 + (size & 1));
        } else if (std::string(tag, 4) == "data") {
            std::vector<int16_t> pcm(size / (2 * channels));
            for (auto& sample : pcm) {
                int sum = 0;
                for (unsigned c = 0; c < channels; ++c) {
                    int const raw = number(2);
                    sum += raw < 32768 ? raw : raw - 65536;
                }
                sample = sum / int(channels);
            }
            if (rate == 0) throw std::runtime_error("invalid WAV sample rate");
            if (rate == sample_rate_hz || pcm.empty()) return pcm;
            std::vector<int16_t> converted(
                uint64_t(pcm.size()) * sample_rate_hz / rate);
            for (size_t i = 0; i < converted.size(); ++i) {
                double const position = double(i) * rate / sample_rate_hz;
                size_t const at = static_cast<size_t>(position);
                double const fraction = position - at;
                converted[i] = static_cast<int16_t>(std::lround(
                    pcm[at] * (1.0 - fraction) +
                    pcm[std::min(at + 1, pcm.size() - 1)] * fraction));
            }
            return converted;
        } else file.ignore(uint64_t(size) + (size & 1));
    }
}
} // namespace

int main(int argc, char** argv)
try
{
    char const* const slash = std::strrchr(argv[0], '/');
    exe_name = slash ? slash + 1 : argv[0];
    if(argc>=2 && std::strcmp(argv[1],"--usb-bench")==0) {
        if(argc<3 || argc>4) return usage(EX_USAGE);
        unsigned seconds=5;
        if(argc==4) {
            std::string arg=argv[3];
            if(arg.empty() || arg.find_first_not_of("0123456789")!=std::string::npos || arg.size()>2)
                return usage(EX_USAGE);
            seconds=static_cast<unsigned>(std::stoul(arg));
        }
        auto result=voice_board_usb_benchmark(argv[2],seconds);
        (result ? std::cout : std::cerr) << result.message << '\n';
        return result ? EX_OK : EX_IOERR;
    }
    auto const base_path = std::filesystem::path(argv[0]).parent_path();
    auto const orchestra_path =
        base_path / "orch01.vc_SV001.wav";
    auto const sine_path = base_path / "sample.wav";
    auto const square_path = base_path / "square.wav";
    auto const sawtooth_path = base_path / "sawtooth.wav";
    auto const triangle_path = base_path / "triangle.wav";
    // Keep argument parsing local; all host transport code lives in voicebd.cpp.
    voice_board_config_t config;
    int positional=0;
    for (int i=1;i<argc;++i) {
        if (std::strcmp(argv[i],"-h")==0 || std::strcmp(argv[i],"--help")==0) return usage(EX_OK);
        if (std::strcmp(argv[i],"--usb-port")==0 || std::strcmp(argv[i],"--rs485-port")==0) {
            bool usb=std::strcmp(argv[i],"--usb-port")==0;
            if(++i==argc) return usage(EX_USAGE);
            (usb ? config.usb_port : config.rs485_port)=argv[i]; continue;
        }
        if (argv[i][0]=='-') return usage(EX_USAGE);
        argv[positional++]=argv[i];
    }
    argc=positional;
    if (argc < 1 || argc > 2) return usage(EX_USAGE);
    auto const pcm = read_wav(argv[0]);
    if (argc == 2) config.bec_file = argv[1];
    auto check = [](voice_board_result_t const& result) {
        if (!result) throw std::runtime_error(result.message);
    };
    voice_board_t board;
    check(board.open(config));
    for (uint8_t voice = 0; voice < voice_board_t::voice_count; ++voice) {
        check(board.load_script(voice, config.bec_file));
        std::cout << "load_script: voice " << unsigned(voice)
                  << " loaded " << config.bec_file << '\n';
    }
    check(board.load_sample(0, pcm));
    RtMidiIn midi;
    midi.ignoreTypes(true, true, true);
    if (midi.getPortCount() == 0)
        throw std::runtime_error("No MIDI input ports found");
    midi.openPort(0);
    std::cout << "MIDI 0: " << midi.getPortName(0) << '\n';
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << "Ready. MIDI voices: " << num_voices
              << "; Space stops all notes/reset state; R compiles/uploads series2.be; A tests replacement during attack; Ctrl+C to exit.\n";
    terminal_keys keyboard;
    // Used only in polyphonic mode: MIDI channel/key owning each voice.
    std::array<int, num_voices> keys;
    keys.fill(-1);
    unsigned next = 0;
    std::vector<unsigned char> message;
    while (!stopped) {
        try {
            int const keypress = keyboard.read_key();
            switch (keypress) {
                case 'r':
                case 'R': {
                    check(board.all_notes_off());
                    keys.fill(-1);
                    next = 0;
                    std::cout << "Compiling series2.be..." << std::endl;
#if defined(__linux__) && defined(__aarch64__)
                    char const* command = "./berry.linux-arm64 series2.be -o series2.bec";
#else
                    char const* command = "./berry series2.be -o series2.bec";
#endif
                    if (std::system(command) == 0) {
                        for (uint8_t voice = 0; voice < voice_board_t::voice_count; ++voice)
                            check(board.load_script(voice, "series2.bec"));
                        std::cout << "series2.bec loaded into voices 0-7." << std::endl;
                    } else {
                        std::cerr << "Compilation failed; no program uploaded.\n";
                    }
                    do { midi.getMessage(&message); } while (!message.empty());
                    break;
                }
                case ' ': {
                    check(board.all_notes_off());
                    keys.fill(-1);
                    next = 0;
                    std::cout << "All notes stopped; Berry state reset.\n";
                    break;
                }
                case 'a':
                case 'A': {
                    std::cout << "Attack replacement test: square to sawtooth..." << std::endl;
                    auto const original = read_wav(square_path.c_str());
                    auto const replacement = read_wav(sine_path.c_str());
                    check(board.load_sample(0, original, sample_rate_hz, sample_root_hz));
                    check(board.note_on(0, 0, 12, 100));
                    // Key 12 stretches the 512-sample attack to about 171 ms.
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    check(board.note_off(0));
                    keys[0] = -1;
                    std::cout << "Attack replacement test finished. Play MIDI to check recovery.\n";
                    break;
                }
                case 'o':
                case 'O': {
                    std::cout << "Loading orchestral sample into sample 0...\n";
                    auto const replacement = read_wav(orchestra_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 's':
                case 'S': {
                    std::cout << "Loading sine wave sample into sample 0...\n";
                    auto const replacement = read_wav(sine_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 'p':
                case 'P': {
                    std::cout << "Loading square wave sample into sample 0...\n";
                    auto const replacement = read_wav(square_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 't':
                case 'T': {
                    std::cout << "Loading triangle wave sample into sample 0...\n";
                    auto const replacement = read_wav(triangle_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 'w':
                case 'W': {
                    std::cout << "Loading sawtooth wave sample into sample 0...\n";
                    auto const replacement = read_wav(sawtooth_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                default:
                    break;
            }

            midi.getMessage(&message);
            if (message.size() >= 3) {
                unsigned const type = message[0] & 0xf0;
                if constexpr (num_voices == 1) {
                    // No key tracking: even releases after Space reach the board.
                    if (type == 0x90 && message[2]) {
                        check(board.note_on(0, 0, message[1], message[2]));
                        print_key_demand(0, message[1]);
                    } else if (type == 0x80 || (type == 0x90 && !message[2])) {
                        check(board.note_off(0));
                    }
                } else {
                    int const key = ((message[0] & 15) << 8) | message[1];
                    if (type == 0x90 && message[2]) {
                        unsigned voice = next;
                        // Prefer an unassigned voice; otherwise steal in round-robin order.
                        for (unsigned i = 0; i < num_voices; ++i) {
                            unsigned const candidate = (next + i) % num_voices;
                            if (keys[candidate] < 0) { voice = candidate; break; }
                        }
                        next = (voice + 1) % num_voices;
                        keys[voice] = -1;
                        check(board.note_on(voice, 0, message[1], message[2]));
                        print_key_demand(voice, message[1]);
                        keys[voice] = key;
                    } else if (type == 0x80 || (type == 0x90 && !message[2])) {
                        for (unsigned voice = 0; voice < num_voices; ++voice) {
                            if (keys[voice] == key) {
                                keys[voice] = -1;
                                check(board.note_off(voice));
                            }
                        }
                    }
                }
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (std::exception const& error) {
            std::cerr << exe_name << ": " << error.what()
                      << "; fix series2.be and press R to reload.\n";
        }
    }
    board.close();
}
catch (std::exception const& error) {
    std::cerr << exe_name << ": " << error.what() << '\n';
    return EX_SOFTWARE;
}

#endif // VOICEBD_TRANSPORT_TEST
