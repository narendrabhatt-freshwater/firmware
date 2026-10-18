/* Test-only, bounded callback trace. Never log or allocate on the audio thread. */
#include "audio.h"
#include "voice.h"
#include "stream.h"
#include "serialdevices.h"
#include <stdio.h>
#include <stdlib.h>

struct RenderTrace {
    uint64_t begin, end;
    uint32_t count, holds_before, holds_after, fill[8];
};
static RenderTrace trace[50000];
static unsigned trace_count;
static bool trace_full;
static void observe_render(int32_t *out, uint32_t count)
{
    if (trace_count == 50000) {
        trace_full = true;
        render_audio_samples(out, count);
        return;
    }
    RenderTrace &row = trace[trace_count++];
    row.begin = get_simulator_time_ns(); row.count = count;
    row.holds_before = get_voice_underrun_count();
    for (unsigned i=0; i<8; ++i) row.fill[i] = get_current_stream_buffered_samples(i);
    render_audio_samples(out, count);
    row.holds_after = get_voice_underrun_count(); row.end = get_simulator_time_ns();
}
#define render_audio_samples observe_render
#include "audio_device.cpp"
#undef render_audio_samples
#define main simulator_main
#include "pty_main.cpp"
#undef main
int main(int argc, char **argv)
{
    int result = simulator_main(argc, argv);
    std::string path = std::string(getenv("CHANNEL_TEST_DIRECTORY")) + "/render.csv";
    FILE *file = fopen(path.c_str(), "w");
    if (!file) return 1;
    fprintf(file, "begin_ns,end_ns,frames,holds_before,holds_after,fill0,fill1,fill2,fill3,fill4,fill5,fill6,fill7\n");
    for (unsigned i=0; i<trace_count; ++i) {
        const RenderTrace &r = trace[i];
        fprintf(file, "%llu,%llu,%u,%u,%u", (unsigned long long)r.begin,
                (unsigned long long)r.end, r.count, r.holds_before, r.holds_after);
        for (unsigned fill : r.fill) fprintf(file, ",%u", fill);
        fputc('\n', file);
    }
    fclose(file);
    if (trace_full) fprintf(stderr, "test: render trace capacity reached\n");
    return result;
}
