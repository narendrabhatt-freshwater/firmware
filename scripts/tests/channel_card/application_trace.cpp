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

#include "audio.h"
#include "channel.h"
#include "voice.h"
#include "samples.h"
#include "stream.h"
#include "filter.h"
#include "usb.h"
#include "backend_model.h"
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <fstream>
#include <iterator>
#include <string.h>

static void emit(const void *p,size_t n) { assert(fwrite(p,1,n,stdout)==n); }
static void render(unsigned count,unsigned partition) {
    int32_t out[512];
    while(count) {
        unsigned n=std::min(count,partition);
#ifdef LEGACY_API
        assert(n%48==0);
        for(unsigned i=0;i<n;i+=48) {
            int32_t stereo[96]; render_audio_block(stereo,48);
            for(unsigned j=0;j<48;++j) out[i+j]=stereo[j*2];
        }
#else
        if(partition==480) { // Test-only oracle: original boundary ordering.
            assert(n%48==0);
            for(unsigned i=0;i<n;i+=48) {
                begin_voice_audio_block();
                for(unsigned j=0;j<48;++j) out[i+j]=mix_next_audio_sample();
                end_voice_audio_block();
            }
        } else {
            render_audio_samples(nullptr,0);
            render_audio_samples(out,n);
        }
#endif
        emit(out,n*sizeof(*out)); count-=n;
    }
}
static void supply() {
    int8_t pcm[1024]; for(unsigned i=0;i<1024;++i) pcm[i]=(int8_t)((i*13)%127-63);
    for(unsigned v=0;v<8;++v) {
        auto current=get_current_stream_session(v);
        if(current!=255 && get_current_stream_free_samples(v)>=sizeof pcm)
            assert(write_stream_body_samples(v,current,pcm,sizeof pcm)==1);
        if(has_pending_voice_stream(v) && get_stream_free_samples(v)>=sizeof pcm)
            assert(write_stream_body_samples(v,get_target_stream_session(v),pcm,sizeof pcm)==1);
    }
}
int main(int argc,char **argv) {
    assert(argc==3);
    unsigned partition=(unsigned)atoi(argv[2]); assert(partition && partition<=512);
    init_usb_protocol(); init_channel_card();
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<char> program{std::istreambuf_iterator<char>(input),{}}; assert(!program.empty());
    uint8_t attack[512]; for(unsigned i=0;i<512;++i) attack[i]=(uint8_t)(i%127-63);
    assert(load_sample_data(0,attack,512)==0);
    assert(load_sample_data(ATTACK_BANK_WAVETABLE_FIRST,attack,512)==0);
    for(unsigned v=0;v<8;++v) {
        assert(begin_voice_script_upload(v)==0);
        assert(append_voice_script_upload_bytes(v,program.data(),program.size())==0);
        assert(commit_voice_script_upload(v)==0);
        assert(set_voice_filter_cutoff_hz(v,700+v*200)==0);
    }
    for(unsigned v=0;v<8;++v) assert(queue_streamed_sample_note(v,0,48+v*3,100,1)==0);
    for(unsigned step=0;step<90;++step) {
        if(step==12 && get_voice_script_fault_count(0)==0) assert(queue_streamed_sample_note(0,0,72,127,2)==0);
        if(step==30) for(unsigned v=0;v<8;++v) if(get_voice_script_fault_count(v)==0) assert(queue_voice_note_off(v)==0);
        if(step==60) request_stop_all_voices();
        if(step==64) for(unsigned v=0;v<8;++v) if(get_voice_script_fault_count(v)==0) assert(queue_streamed_sample_note(v,0,60,127,3)==0);
        if(step<45 || step>55) supply(); // Include starvation and refill.
        render(480,partition);
        if(step==1 && strstr(argv[1],"fault.bec")) {
            assert(get_voice_script_fault_count(0)>0 && !is_voice_active(0));
        }
        for(unsigned v=0;v<8;++v) {
            float amp=get_voice_amplitude(v); emit(&amp,sizeof amp);
            uint32_t state[]={is_voice_active(v),get_current_stream_session(v),
                get_stream_free_samples(v),get_voice_script_fault_count(v)};
            emit(state,sizeof state);
        }
    }
    char command[]="c:vm"; execute_usb_command(command);
    emit(model::rs_tx.data(),model::rs_tx.size());
    return 0;
}
