#ifndef SCRIPT_BERRY_BACKEND_H
#define SCRIPT_BERRY_BACKEND_H

#include "script/script_runtime.h"
#include <setjmp.h>

#define SCRIPT_BERRY_ARENA_SIZE (16u * 1024u)
#define SCRIPT_BERRY_LOAD_HEAP_SIZE (12u * 1024u)
#define SCRIPT_BERRY_INSTRUCTION_BUDGET 8192u

typedef union {
    long double alignment;
    uint8_t bytes[SCRIPT_BERRY_ARENA_SIZE];
} ScriptBerryArena;

typedef struct {
    ScriptBerryArena arena;
    void *vm;
    ScriptRuntime *runtime;
    ScriptBackendMetrics metrics;
    ScriptLifecyclePhase phase;
    ScriptBackendResult pending_fault;
    uint32_t tick_instruction_start;
    uint32_t instruction_budget;
    uint32_t heap_limit;
    jmp_buf abort_jump;
    bool abort_active;
    bool discard_vm;
} ScriptBerryBackend;

void script_berry_backend_init(ScriptBerryBackend *berry, ScriptBackend *backend);

#endif
