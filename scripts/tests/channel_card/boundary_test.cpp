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
#include "usb.h"
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc,char **argv)
{
    assert(argc==2);
    init_usb_protocol(); init_channel_card();
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<char> program{std::istreambuf_iterator<char>(in),{}};
    assert(begin_voice_script_upload(0)==0);
    assert(append_voice_script_upload_bytes(0,program.data(),program.size())==0);
    assert(commit_voice_script_upload(0)==0);
    uint8_t pcm[1024]={}; assert(load_sample_data(0,pcm,512)==0);
    int32_t out[256];
    // Command at sample 47 waits until sample 48 begins the next interval.
    render_audio_samples(out,47);
    assert(queue_streamed_sample_note(0,0,60,127,1)==0);
    assert(write_stream_body_samples(0,1,(int8_t*)pcm,1024)==1);
    render_audio_samples(nullptr,0); assert(!is_voice_active(0));
    render_audio_samples(out,1); assert(!is_voice_active(0));
    render_audio_samples(out,1); assert(is_voice_active(0));
    // Clear after sample 48 doesn't run at an arbitrary caller boundary.
    request_stop_all_voices(); render_audio_samples(out,46); assert(is_voice_active(0));
    render_audio_samples(out,1); assert(is_voice_active(0));
    render_audio_samples(out,1); assert(!is_voice_active(0));
    // No output beyond the caller's buffer, including a cross-interval call.
    for(auto &sample:out) sample=0x12345678;
    render_audio_samples(out+1,250);
    assert(out[0]==0x12345678 && out[251]==0x12345678);
    return 0;
}
