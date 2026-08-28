#define _POSIX_C_SOURCE 200809L
#include "script/berry_backend.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t tick;
    uint32_t order;
    char kind[16];
    char name[24];
    float value;
    uint16_t ramp;
} InputEvent;

static int compare_event(const void *a, const void *b)
{
    const InputEvent *lhs = (const InputEvent *)a;
    const InputEvent *rhs = (const InputEvent *)b;
    if (lhs->tick != rhs->tick) return lhs->tick > rhs->tick ? 1 : -1;
    return lhs->order > rhs->order ? 1 : lhs->order < rhs->order ? -1 : 0;
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t hash_outputs(uint64_t hash,
        const float outputs[SCRIPT_MAX_NOTE_SLOTS][SCRIPT_MAX_OUTPUTS], uint8_t count)
{
    for (uint8_t note = 0; note < SCRIPT_MAX_NOTE_SLOTS; ++note) {
        for (uint8_t output = 0; output < count; ++output) {
            uint32_t bits;
            memcpy(&bits, &outputs[note][output], sizeof(bits));
            for (unsigned i = 0; i < 4; ++i) {
                hash ^= (uint8_t)(bits >> (i * 8));
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t lhs = *(const uint64_t *)a, rhs = *(const uint64_t *)b;
    return lhs > rhs ? 1 : lhs < rhs ? -1 : 0;
}

static int load_events(const char *path, InputEvent **events, size_t *count)
{
    FILE *file = fopen(path, "r");
    char line[256];
    size_t capacity = 0;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        InputEvent e = {0};
        char extra;
        int fields;
        if (line[0] == '#' || line[0] == '\n') continue;
        fields = sscanf(line, "%u %15s %23s %f %hu %c", &e.tick, e.kind, e.name,
                        &e.value, &e.ramp, &extra);
        if ((strcmp(e.kind, "control") == 0 && (fields < 4 || fields > 5)) ||
            (strcmp(e.kind, "note_on") == 0 && fields != 3) ||
            (strcmp(e.kind, "note_off") == 0 && fields != 3) ||
            (strcmp(e.kind, "note_restart") == 0 && fields != 3) ||
            (strcmp(e.kind, "control") != 0 &&
             strcmp(e.kind, "note_on") != 0 &&
             strcmp(e.kind, "note_off") != 0 &&
             strcmp(e.kind, "note_restart") != 0)) {
            fclose(file); return -2;
        }
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            InputEvent *grown = (InputEvent *)realloc(*events, capacity * sizeof(**events));
            if (!grown) { fclose(file); return -3; }
            *events = grown;
        }
        e.order = (uint32_t)*count;
        (*events)[(*count)++] = e;
    }
    fclose(file);
    qsort(*events, *count, sizeof(**events), compare_event);
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET))
        return -1;
    *data = (uint8_t *)malloc((size_t)length);
    if (!*data || fread(*data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); return -1;
    }
    fclose(file); *size = (size_t)length; return 0;
}

int main(int argc, char **argv)
{
    const char *program = NULL, *events_path = NULL, *csv_path = NULL;
    uint32_t ticks = 0, sequence = 1;
    bool realtime = false;
    uint8_t *container = NULL;
    size_t container_size = 0, event_count = 0, event_cursor = 0;
    InputEvent *events = NULL;
    FILE *csv = NULL;
    ScriptRuntime runtime;
    ScriptBerryBackend berry;
    ScriptBackend backend;
    ScriptBackendMetrics bm, loaded_bm;
    uint64_t hash = UINT64_C(1469598103934665603), total_ns = 0;
    uint32_t host_overruns = 0;
    uint64_t *durations;

    if (argc >= 2) program = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--ticks") == 0 && ++i < argc) ticks = (uint32_t)strtoul(argv[i], NULL, 10);
        else if (strcmp(argv[i], "--events") == 0 && ++i < argc) events_path = argv[i];
        else if (strcmp(argv[i], "--csv") == 0 && ++i < argc) csv_path = argv[i];
        else if (strcmp(argv[i], "--realtime") == 0) realtime = true;
        else { fprintf(stderr, "usage: control_runtime_sim PROGRAM.fwsc --ticks N [--events FILE] [--csv FILE] [--realtime]\n"); return 2; }
    }
    if (!program || ticks == 0 || read_file(program, &container, &container_size)) {
        fprintf(stderr, "control_runtime_sim: invalid program or tick count\n"); return 2;
    }
    if (events_path && load_events(events_path, &events, &event_count)) {
        fprintf(stderr, "control_runtime_sim: invalid event file\n"); return 2;
    }
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) { fprintf(stderr, "control_runtime_sim: %s\n", strerror(errno)); return 2; }
        fputs("tick,note,output,value\n", csv);
    }
    durations = (uint64_t *)calloc(ticks, sizeof(*durations));
    if (!durations) return 2;
    script_berry_backend_init(&berry, &backend);
    script_runtime_init(&runtime, &backend);
    if (!script_runtime_upload_begin(&runtime) ||
        !script_runtime_upload_feed(&runtime, container, container_size) ||
        !script_runtime_upload_commit(&runtime)) {
        fprintf(stderr, "control_runtime_sim: upload failed (fault %d)\n", runtime.fault); return 1;
    }
    for (uint32_t tick = 0; tick < ticks; ++tick) {
        while (event_cursor < event_count && events[event_cursor].tick == tick) {
            InputEvent *e = &events[event_cursor++];
            if (strcmp(e->kind, "control") == 0) {
                int id = script_runtime_find_control(&runtime, e->name);
                if (id < 0 || !script_runtime_enqueue_control(&runtime, runtime.generation,
                        sequence++, (uint8_t)id, e->value, tick, e->ramp)) {
                    fprintf(stderr, "control_runtime_sim: rejected event at tick %u\n", tick); return 1;
                }
            } else {
                uint8_t note = (uint8_t)strtoul(e->name, NULL, 10);
                bool ok;
                if (strcmp(e->kind, "note_on") == 0) {
                    ok = script_runtime_enqueue_note_on(&runtime, runtime.generation,
                                                        sequence++, note, tick);
                } else if (strcmp(e->kind, "note_off") == 0) {
                    ok = script_runtime_enqueue_note_off(&runtime, runtime.generation,
                                                         sequence++, note, tick);
                } else {
                    ok = script_runtime_enqueue_note_restart(&runtime,
                            runtime.generation, sequence++, note, tick);
                }
                if (!ok) { fprintf(stderr, "control_runtime_sim: rejected event at tick %u\n", tick); return 1; }
            }
        }
        uint64_t start = now_ns();
        if (!script_runtime_tick(&runtime)) {
            fprintf(stderr, "control_runtime_sim: runtime fault %d at tick %u\n", runtime.fault, tick); return 1;
        }
        durations[tick] = now_ns() - start;
        total_ns += durations[tick];
        if (durations[tick] > UINT64_C(1000000)) ++host_overruns;
        const float (*out)[SCRIPT_MAX_OUTPUTS] = script_runtime_outputs(&runtime);
        hash = hash_outputs(hash, out, runtime.output_count);
        if (csv) for (uint8_t note = 0; note < SCRIPT_MAX_NOTE_SLOTS; ++note)
            for (uint8_t output = 0; output < runtime.output_count; ++output)
                fprintf(csv, "%u,%u,%u,%.9g\n", tick, note, output,
                        out[note][output]);
        if (realtime && durations[tick] < UINT64_C(1000000)) {
            struct timespec delay = {0, (long)(UINT64_C(1000000) - durations[tick])};
            nanosleep(&delay, NULL);
        }
    }
    if (csv) fclose(csv);
    backend.metrics(backend.context, &loaded_bm);
    script_runtime_stop(&runtime);
    backend.metrics(backend.context, &bm);
    qsort(durations, ticks, sizeof(*durations), compare_u64);
    printf("{\"payload_size\":%u,\"ticks_completed\":%u,\"outputs_generated\":%" PRIu64
           ",\"instruction_last\":%u,\"instruction_total\":%u,\"instruction_max\":%u"
           ",\"host_tick_ns_min\":%" PRIu64 ",\"host_tick_ns_mean\":%.1f,\"host_tick_ns_p99\":%" PRIu64
           ",\"host_tick_ns_max\":%" PRIu64 ",\"arena_current\":%u,\"arena_peak\":%u"
           ",\"arena_largest_free\":%u"
           ",\"alloc_idle\":%u,\"alloc_init\":%u,\"alloc_tick\":%u,\"alloc_destroy\":%u"
           ",\"realloc_idle\":%u,\"realloc_init\":%u,\"realloc_tick\":%u,\"realloc_destroy\":%u"
           ",\"free_idle\":%u,\"free_init\":%u,\"free_tick\":%u,\"free_destroy\":%u"
           ",\"gc_idle\":%u,\"gc_init\":%u,\"gc_tick\":%u,\"gc_destroy\":%u"
           ",\"clamps\":%u,\"faults\":%u,\"upload_failures\":%u"
           ",\"queue_accepted\":%u,\"queue_applied\":%u,\"queue_overflow\":%u"
           ",\"stale_generation\":%u,\"stale_sequence\":%u,\"range_clamps\":%u"
           ",\"host_overruns\":%u,\"supporting_state_bytes\":%zu,\"backend_overhead_bytes\":%zu"
           ",\"subsystem_state_bytes\":%zu"
           ",\"output_hash\":\"%016" PRIx64 "\"}\n",
           container_size >= 16 ? read_u32_le(container + 12) : 0,
           runtime.metrics.ticks_completed, runtime.metrics.outputs_generated,
           bm.instructions_last, bm.instructions_total, bm.instructions_max,
           durations[0], (double)total_ns / ticks, durations[(ticks - 1) * 99 / 100], durations[ticks - 1],
           loaded_bm.arena_current, loaded_bm.arena_peak, loaded_bm.arena_largest_free,
           bm.allocations[SCRIPT_PHASE_IDLE], bm.allocations[SCRIPT_PHASE_INIT],
           bm.allocations[SCRIPT_PHASE_TICK], bm.allocations[SCRIPT_PHASE_DESTROY],
           bm.reallocations[SCRIPT_PHASE_IDLE], bm.reallocations[SCRIPT_PHASE_INIT],
           bm.reallocations[SCRIPT_PHASE_TICK], bm.reallocations[SCRIPT_PHASE_DESTROY],
           bm.frees[SCRIPT_PHASE_IDLE], bm.frees[SCRIPT_PHASE_INIT],
           bm.frees[SCRIPT_PHASE_TICK], bm.frees[SCRIPT_PHASE_DESTROY],
           bm.gc_runs[SCRIPT_PHASE_IDLE], bm.gc_runs[SCRIPT_PHASE_INIT],
           bm.gc_runs[SCRIPT_PHASE_TICK], bm.gc_runs[SCRIPT_PHASE_DESTROY],
           runtime.metrics.clamps, runtime.metrics.faults, runtime.metrics.upload_failures,
           runtime.metrics.queue_accepted,
           runtime.metrics.queue_applied, runtime.metrics.queue_overflow,
           runtime.metrics.stale_generation, runtime.metrics.stale_sequence,
           runtime.metrics.range_clamps, host_overruns, sizeof(runtime),
           sizeof(berry) - SCRIPT_BERRY_ARENA_SIZE, sizeof(runtime) + sizeof(berry), hash);
    free(durations); free(events); free(container);
    return 0;
}
