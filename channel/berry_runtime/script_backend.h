#ifndef SCRIPT_BACKEND_H
#define SCRIPT_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ScriptRuntime;

typedef enum {
    SCRIPT_BACKEND_OK = 0,
    SCRIPT_BACKEND_BAD_BYTECODE,
    SCRIPT_BACKEND_EXCEPTION,
    SCRIPT_BACKEND_OOM,
    SCRIPT_BACKEND_WATCHDOG,
    SCRIPT_BACKEND_ALLOCATION_IN_TICK,
    SCRIPT_BACKEND_GC_IN_TICK,
    SCRIPT_BACKEND_BAD_ABI
} ScriptBackendResult;

typedef enum {
    SCRIPT_PHASE_IDLE = 0,
    SCRIPT_PHASE_INIT,
    SCRIPT_PHASE_TICK,
    SCRIPT_PHASE_DESTROY
} ScriptLifecyclePhase;

typedef struct {
    uint32_t instructions_last;
    uint32_t instructions_total;
    uint32_t instructions_max;
    uint32_t allocations[4];
    uint32_t reallocations[4];
    uint32_t frees[4];
    uint32_t gc_runs[4];
    uint32_t arena_current;
    uint32_t arena_peak;
    uint32_t arena_largest_free;
} ScriptBackendMetrics;

typedef struct ScriptBackend {
    void *context;
    uint8_t *(*begin_upload)(void *context, size_t capacity);
    ScriptBackendResult (*load)(void *context, struct ScriptRuntime *runtime,
                                const uint8_t *bytecode, size_t size);
    ScriptBackendResult (*tick)(void *context);
    void (*stop)(void *context);
    void (*metrics)(void *context, ScriptBackendMetrics *metrics);
} ScriptBackend;

#endif
