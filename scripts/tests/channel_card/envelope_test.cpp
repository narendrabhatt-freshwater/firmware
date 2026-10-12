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

/* Measure the bundled envelope program using sample-clocked rendering.
 * Constant source PCM exposes the gain envelope directly in the output. */
#include "audio.h"
#include "channel.h"
#include "voice.h"
#include "samples.h"
#include "stream.h"
#include "usb.h"
#include <algorithm>
#include <assert.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static unsigned partition;
static std::vector<int32_t> pcm;
static int8_t source[1024];

static void supply()
{
    for (unsigned voice = 0; voice < 8; ++voice) {
        if (has_pending_voice_stream(voice) &&
            get_pending_stream_buffered_samples(voice) < sizeof source &&
            get_stream_free_samples(voice) >= sizeof source)
            assert(write_stream_body_samples(voice, get_target_stream_session(voice),
                                              source, sizeof source) == 1);
        if (get_current_stream_session(voice) != 255 &&
            get_current_stream_buffered_samples(voice) < sizeof source &&
            get_current_stream_free_samples(voice) >= sizeof source)
            assert(write_stream_body_samples(voice, get_current_stream_session(voice),
                                              source, sizeof source) == 1);
    }
}

static void advance(unsigned count, bool refill = true)
{
    int32_t block[1024];
    while (count) {
        if (refill) supply();
        unsigned n = std::min(count, partition);
        render_audio_samples(block, n);
        pcm.insert(pcm.end(), block, block + n);
        count -= n;
    }
}

static void amplitude(unsigned voice, float expected)
{
    float actual = get_voice_amplitude(voice);
    // Single-precision sample accumulation, less than three release samples.
    if (fabsf(actual - expected) > 0.0002f) {
        fprintf(stderr, "voice %u amplitude %.8f, expected %.8f at sample %zu\n",
                voice, actual, expected, pcm.size());
        abort();
    }
}

static void note(unsigned voice, unsigned session)
{
    assert(pcm.size() % 48 == 0);
    assert(queue_streamed_sample_note(voice, 0, 69, 127, session) == 0);
}

static void clear()
{
    request_stop_all_voices();
    advance(96);
    for (unsigned v = 0; v < 8; ++v) {
        assert(!is_voice_active(v) && !has_pending_voice_stream(v));
        amplitude(v, 0);
    }
}

static void silent_since(size_t first)
{
    assert(std::all_of(pcm.begin() + first, pcm.end(), [](int32_t s) { return s == 0; }));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    partition = (unsigned)atoi(argv[2]);
    assert(partition > 0 && partition <= 1024);
    init_usb_protocol();
    init_channel_card();
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> program{std::istreambuf_iterator<char>(input), {}};
    assert(!program.empty());
    std::fill(std::begin(source), std::end(source), 16);
    assert(load_sample_data(0, reinterpret_cast<uint8_t *>(source), 512) == 0);
    set_sample_root_frequency_hz(0, 440);
    for (unsigned v = 0; v < 8; ++v) {
        assert(begin_voice_script_upload(v) == 0);
        assert(append_voice_script_upload_bytes(v, program.data(), program.size()) == 0);
        assert(commit_voice_script_upload(v) == 0);
    }

    // Note-off before the 998-sample start gate cancels the pending note.
    note(0, 1);
    advance(480, false);
    assert(!is_voice_active(0) && has_pending_voice_stream(0));
    assert(queue_voice_note_off(0) == 0);
    advance(480, false);
    assert(!has_pending_voice_stream(0));
    silent_since(0);

    // Full attack, sustain and a streamed release, using the actual example.
    note(0, 2);
    advance(1200); amplitude(0, 0.5f); assert(is_voice_active(0));
    advance(1248); amplitude(0, 1);
    size_t sustain = pcm.size();
    advance(4800); amplitude(0, 1);
    assert(*std::min_element(pcm.begin() + sustain, pcm.end()) > 0);
    int32_t full = pcm.back();
    assert(queue_voice_note_off(0) == 0);
    size_t release = pcm.size();
    advance(6000); amplitude(0, 0.5f); assert(is_voice_active(0));
    assert(std::abs(pcm.back() - full / 2) < full / 1000);
    advance(5952); amplitude(0, 0.004f); assert(is_voice_active(0));
    advance(96); amplitude(0, 0); assert(!is_voice_active(0));
    for (size_t i = release + 1; i < pcm.size(); ++i) assert(pcm[i] <= pcm[i - 1]);
    size_t silence = pcm.size(); advance(480); silent_since(silence);

    // Releasing 20 ms into attack starts from 0.4, taking 100 ms at slope 4.
    note(0, 3);
    advance(960); amplitude(0, 0.4f);
    assert(queue_voice_note_off(0) == 0);
    advance(2400); amplitude(0, 0.2f); assert(is_voice_active(0));
    // A repeated key-up must continue the same slope, not restart at full gain.
    assert(queue_voice_note_off(0) == 0);
    advance(2448); amplitude(0, 0); assert(!is_voice_active(0));

    // Retrigger while holding: fade the old session before starting the new one.
    note(0, 4); advance(4800); amplitude(0, 1);
    note(0, 5);
    advance(96); amplitude(0, 1.0f / 3); assert(get_current_stream_session(0) == 4);
    advance(96); assert(get_current_stream_session(0) == 5);
    assert(get_voice_amplitude(0) <= 0.021f);
    advance(2448); amplitude(0, 1);

    // Retrigger during a release: 0.6 -> 0 in 3 ms, then a new 50 ms attack.
    assert(queue_voice_note_off(0) == 0);
    advance(4800); amplitude(0, 0.6f);
    note(0, 6); advance(96); amplitude(0, 0.2f);
    assert(get_current_stream_session(0) == 5);
    advance(96); assert(get_current_stream_session(0) == 6);
    assert(get_voice_amplitude(0) <= 0.021f);
    advance(2448); amplitude(0, 1);

    // Rapid replacement during the crash fade chooses the newest session.
    note(0, 7); advance(48);
    note(0, 8); advance(192);
    assert(get_current_stream_session(0) == 8 && !has_pending_voice_stream(0));
    advance(2448); amplitude(0, 1);

    // Key-up during a steal cancels the replacement and releases the old note.
    note(0, 9); advance(48);
    assert(has_pending_voice_stream(0));
    assert(queue_voice_note_off(0) == 0); advance(48);
    assert(!has_pending_voice_stream(0) && get_current_stream_session(0) == 8);
    advance(12048); assert(!is_voice_active(0));

    // Clearing during a crash fade must not start the queued replacement later.
    note(0, 10); advance(2448);
    note(0, 11); advance(48);
    clear();
    silence = pcm.size(); advance(14400); silent_since(silence);

    // Hard clear cancels release/ramp-end callbacks and keeps loaded programs.
    note(0, 12); advance(2448);
    assert(queue_voice_note_off(0) == 0); advance(2400);
    clear();
    silence = pcm.size(); advance(14400); silent_since(silence);
    note(0, 13); advance(2448); amplitude(0, 1);
    clear();

    // Independent releases in all eight voices must not affect held voices.
    for (unsigned v = 0; v < 8; ++v) note(v, 14);
    advance(2448);
    for (unsigned v = 0; v < 8; ++v) amplitude(v, 1);
    for (unsigned v = 0; v < 4; ++v) assert(queue_voice_note_off(v) == 0);
    advance(6000);
    for (unsigned v = 0; v < 8; ++v) amplitude(v, v < 4 ? 0.5f : 1.0f);
    for (unsigned v = 4; v < 8; ++v) assert(queue_voice_note_off(v) == 0);
    advance(6048);
    for (unsigned v = 0; v < 4; ++v) assert(!is_voice_active(v));
    for (unsigned v = 4; v < 8; ++v) amplitude(v, 0.496f);
    advance(6000);
    for (unsigned v = 0; v < 8; ++v) {
        assert(!is_voice_active(v) && !has_pending_voice_stream(v));
        assert(get_voice_script_fault_count(v) == 0);
    }
    silence = pcm.size(); advance(480); silent_since(silence);
    assert(get_voice_underrun_count() == 0 && get_stream_dropped_block_count() == 0);
    assert(fwrite(pcm.data(), sizeof(int32_t), pcm.size(), stdout) == pcm.size());
    return 0;
}
