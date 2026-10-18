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
#include "filter.h"
#include "oscillator.h"
#include "vm_channel.h"
#include <cmath>
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>

// Independent signal properties, not another implementation of the DSP.
static void check_signal_paths()
{
    init_voice_filters();
    for (unsigned voice = 0; voice < 8; ++voice) {
        assert(filter_voice_sample(voice, 123456789) == 123456789);
        assert(set_voice_filter_cutoff_hz(voice, 1000) == 0);
        assert(set_voice_filter_pitch_tracking(voice, 1) == 0);
        update_voice_filter_note_frequency(voice, 2 * NOTE_FILTER_F_REF_HZ);
        assert(std::abs(get_voice_filter_cutoff_hz(voice) - 2000) < 0.001);
        int32_t impulse[128];
        for (unsigned i = 0; i < 128; ++i)
            impulse[i] = filter_voice_sample(voice, i == 0 ? 1000000 : 0);
        reset_voice_filter(voice);
        for (unsigned i = 0; i < 128; ++i)
            assert(filter_voice_sample(voice, i == 0 ? 1000000 : 0) == impulse[i]);
        assert(set_voice_filter_cutoff_hz(voice, 0) == 0);
        assert(filter_voice_sample(voice, -123456789) == -123456789);
    }
    // A constant +0.5 wavetable makes modulation independent of phase/pitch.
    uint8_t wave[16];
    for (auto &sample : wave) sample = 64;
    assert(load_sample_data(WAVETABLE_OSC_WAVE_FIRST, wave, sizeof wave) == 0);
    for (unsigned voice = 0; voice < 8; ++voice) {
        for (unsigned repeat = 0; repeat < 64; ++repeat) {
            begin_pending_oscillators(voice);
            uint32_t modulator, carrier;
            assert(add_pending_oscillator(voice, 0, 100, &modulator) == 0);
            assert(add_pending_oscillator(voice, 0, 440, &carrier) == 0);
            assert(add_pending_oscillator_route(voice, modulator, carrier,
                       FW_VM_CHANNEL_ROUTE_FREQUENCY, 100) == 0);
            assert(add_pending_oscillator_route(voice, carrier, FW_VM_CHANNEL_TARGET_SAMPLE,
                       FW_VM_CHANNEL_ROUTE_AMPLITUDE, 1) == 0);
            assert(finalize_pending_oscillators(voice) == 0);
            activate_pending_oscillators(voice);
            float frequency, amplitude;
            apply_oscillator_sample_modulation(voice, &frequency, &amplitude);
            assert(frequency == 0 && amplitude == 0.75f);
            assert(mix_voice_oscillators(voice, 1000000) == 750000);
            // An invalid pending cycle must not replace the sounding graph.
            begin_pending_oscillators(voice);
            uint32_t a, b;
            assert(add_pending_oscillator(voice, 0, 100, &a) == 0);
            assert(add_pending_oscillator(voice, 0, 200, &b) == 0);
            assert(add_pending_oscillator_route(voice, a, b,
                       FW_VM_CHANNEL_ROUTE_FREQUENCY, 1) == 0);
            assert(add_pending_oscillator_route(voice, b, a,
                       FW_VM_CHANNEL_ROUTE_FREQUENCY, 1) == 0);
            assert(finalize_pending_oscillators(voice) != 0);
            activate_pending_oscillators(voice);
            apply_oscillator_sample_modulation(voice, &frequency, &amplitude);
            assert(amplitude == 0.75f);
            discard_pending_oscillators(voice);
            stop_voice_oscillators(voice);
            apply_oscillator_sample_modulation(voice, &frequency, &amplitude);
            assert(frequency == 0 && amplitude == 1);
            assert(mix_voice_oscillators(voice, 1000000) == 1000000);
        }
    }
    init_voice_filters();
}

int main(int argc,char **argv)
{
    assert(argc==2);
    init_usb_protocol(); init_channel_card();
    check_signal_paths();
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
    assert(!has_pending_voice_stream(0) && has_prepared_voice_command(0));
    render_audio_samples(nullptr,0); assert(!is_voice_active(0));
    render_audio_samples(out,1); assert(!has_pending_voice_stream(0));
    render_audio_samples(out,1); // sample 48 publishes the prepared session
    assert(has_pending_voice_stream(0) && !has_prepared_voice_command(0));
    assert(!is_voice_active(0) && get_target_stream_session(0) == 1);
    assert(write_stream_body_samples(0,1,(int8_t*)pcm,1024)==1);
    render_audio_samples(out,47); assert(!is_voice_active(0));
    render_audio_samples(out,1); assert(is_voice_active(0)); // ready at sample 96
    // Clear after sample 96 doesn't run at an arbitrary caller boundary.
    request_stop_all_voices(); render_audio_samples(out,46); assert(is_voice_active(0));
    render_audio_samples(out,1); assert(is_voice_active(0));
    render_audio_samples(out,1); assert(!is_voice_active(0));
    // No output beyond the caller's buffer, including a cross-interval call.
    for(auto &sample:out) sample=0x12345678;
    render_audio_samples(out+1,250);
    assert(out[0]==0x12345678 && out[251]==0x12345678);
    return 0;
}
