#include "script/berry_backend.h"

#include "be_vm.h"
#include "be_exec.h"
#include "be_vector.h"
#include "berry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t size;
    uint32_t used;
} ArenaBlock;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
} MemoryFile;

static ScriptBerryBackend *active_backend;
static MemoryFile memory_file;

static size_t align8(size_t value) { return (value + 7u) & ~(size_t)7u; }

static ArenaBlock *first_block(ScriptBerryBackend *b)
{
    return (ArenaBlock *)(void *)b->arena.bytes;
}

static ArenaBlock *next_block(ScriptBerryBackend *b, ArenaBlock *block)
{
    uint8_t *next = (uint8_t *)(void *)(block + 1) + block->size;
    uint8_t *end = b->arena.bytes + b->heap_limit;
    return next + sizeof(ArenaBlock) <= end ? (ArenaBlock *)(void *)next : NULL;
}

static void arena_reset(ScriptBerryBackend *b, uint32_t limit)
{
    ArenaBlock *block;
    memset(b->arena.bytes, 0, SCRIPT_BERRY_ARENA_SIZE);
    b->heap_limit = limit;
    block = first_block(b);
    block->size = limit - (uint32_t)sizeof(*block);
    block->used = 0;
    memset(&b->metrics, 0, sizeof(b->metrics));
    b->metrics.arena_largest_free = block->size;
}

static void update_arena_metrics(ScriptBerryBackend *b)
{
    uint32_t current = 0, largest = 0;
    for (ArenaBlock *block = first_block(b); block; block = next_block(b, block)) {
        if (block->used) current += block->size;
        else if (block->size > largest) largest = block->size;
    }
    b->metrics.arena_current = current;
    if (current > b->metrics.arena_peak) b->metrics.arena_peak = current;
    b->metrics.arena_largest_free = largest;
}

static void *arena_malloc(ScriptBerryBackend *b, size_t requested)
{
    const size_t size = align8(requested);
    if (!requested) return NULL;
    if (b->phase == SCRIPT_PHASE_TICK) {
        ++b->metrics.allocations[SCRIPT_PHASE_TICK];
        b->pending_fault = SCRIPT_BACKEND_ALLOCATION_IN_TICK;
        b->discard_vm = true;
        if (b->abort_active) longjmp(b->abort_jump, 1);
        return NULL;
    }
    for (ArenaBlock *block = first_block(b); block; block = next_block(b, block)) {
        if (!block->used && block->size >= size) {
            if (block->size >= size + sizeof(ArenaBlock) + 8u) {
                ArenaBlock *split = (ArenaBlock *)(void *)((uint8_t *)(block + 1) + size);
                split->size = block->size - (uint32_t)size - (uint32_t)sizeof(*split);
                split->used = 0;
                block->size = (uint32_t)size;
            }
            block->used = 1;
            ++b->metrics.allocations[b->phase];
            update_arena_metrics(b);
            return block + 1;
        }
    }
    return NULL;
}

static void arena_free(ScriptBerryBackend *b, void *ptr)
{
    ArenaBlock *block;
    if (!ptr) return;
    block = (ArenaBlock *)ptr - 1;
    if ((uint8_t *)(void *)block < b->arena.bytes ||
        (uint8_t *)(void *)block >= b->arena.bytes + b->heap_limit || !block->used) return;
    block->used = 0;
    ++b->metrics.frees[b->phase];
    for (ArenaBlock *it = first_block(b); it;) {
        ArenaBlock *next = next_block(b, it);
        if (next && !it->used && !next->used) {
            it->size += (uint32_t)sizeof(*next) + next->size;
        } else {
            it = next;
        }
    }
    update_arena_metrics(b);
}

void *script_berry_malloc(size_t size)
{
    return active_backend ? arena_malloc(active_backend, size) : NULL;
}

void script_berry_free(void *ptr)
{
    if (active_backend) arena_free(active_backend, ptr);
}

void *script_berry_realloc(void *ptr, size_t size)
{
    ArenaBlock *old;
    void *fresh;
    if (!active_backend) return NULL;
    if (!ptr) return arena_malloc(active_backend, size);
    if (!size) { arena_free(active_backend, ptr); return NULL; }
    ++active_backend->metrics.reallocations[active_backend->phase];
    if (active_backend->phase == SCRIPT_PHASE_TICK) {
        active_backend->pending_fault = SCRIPT_BACKEND_ALLOCATION_IN_TICK;
        active_backend->discard_vm = true;
        if (active_backend->abort_active) longjmp(active_backend->abort_jump, 1);
        return NULL;
    }
    old = (ArenaBlock *)ptr - 1;
    if (old->size >= align8(size)) return ptr;
    fresh = arena_malloc(active_backend, size);
    if (fresh) {
        memcpy(fresh, ptr, old->size);
        arena_free(active_backend, ptr);
    }
    return fresh;
}

void be_writebuffer(const char *buffer, size_t length)
{
    (void)buffer;
    (void)length;
}

char *be_readstring(char *buffer, size_t size)
{
    (void)buffer; (void)size; return NULL;
}

void *be_fopen(const char *filename, const char *modes)
{
    (void)modes;
    if (strcmp(filename, "@fwsc-memory") != 0 || !memory_file.data) return NULL;
    memory_file.position = 0;
    return &memory_file;
}

int be_fclose(void *file) { return file == &memory_file ? 0 : -1; }
size_t be_fwrite(void *file, const void *buffer, size_t length)
{ (void)file; (void)buffer; (void)length; return 0; }
size_t be_fread(void *file, void *buffer, size_t length)
{
    size_t available;
    if (file != &memory_file) return 0;
    available = memory_file.size - memory_file.position;
    if (length > available) length = available;
    memcpy(buffer, memory_file.data + memory_file.position, length);
    memory_file.position += length;
    return length;
}
char *be_fgets(void *file, void *buffer, int size)
{ (void)file; (void)buffer; (void)size; return NULL; }
int be_fseek(void *file, long offset)
{
    if (file != &memory_file || offset < 0 || (size_t)offset > memory_file.size) return -1;
    memory_file.position = (size_t)offset; return 0;
}
long be_ftell(void *file) { return file == &memory_file ? (long)memory_file.position : -1; }
long be_fflush(void *file) { (void)file; return 0; }
size_t be_fsize(void *file) { return file == &memory_file ? memory_file.size : 0; }

static int require_count(bvm *vm, int count)
{
    if (be_top(vm) != count) {
        if (active_backend->runtime)
            active_backend->runtime->fault = SCRIPT_FAULT_BAD_NATIVE_ARGUMENT;
        be_raise(vm, "value_error", "invalid argument count");
    }
    return 1;
}

static void native_argument_error(bvm *vm, const char *message)
{
    if (active_backend->runtime)
        active_backend->runtime->fault = SCRIPT_FAULT_BAD_NATIVE_ARGUMENT;
    be_raise(vm, "value_error", message);
}

static bint checked_int(bvm *vm, int index, bint minimum, bint maximum)
{
    bint value;
    if (!be_isint(vm, index)) native_argument_error(vm, "integer argument required");
    value = be_toint(vm, index);
    if (value < minimum || value > maximum)
        native_argument_error(vm, "integer argument out of range");
    return value;
}

static float checked_number(bvm *vm, int index)
{
    if (!be_isnumber(vm, index)) native_argument_error(vm, "numeric argument required");
    return (float)be_toreal(vm, index);
}

static float checked_finite_number(bvm *vm, int index)
{
    float value = checked_number(vm, index);
    if (!isfinite(value)) native_argument_error(vm, "finite argument required");
    return value;
}

static int native_configure_outputs(bvm *vm)
{
    ScriptRuntime *r = active_backend->runtime;
    require_count(vm, 1);
    if (script_runtime_configure_outputs(r,
            (uint8_t)checked_int(vm, 1, 1, SCRIPT_MAX_OUTPUTS)) != 0)
        native_argument_error(vm, "invalid output count");
    be_pushnil(vm); be_return(vm);
}

static int native_define_control(bvm *vm)
{
    int id;
    require_count(vm, 5);
    if (!be_isstring(vm, 1)) native_argument_error(vm, "control name must be a string");
    id = script_runtime_define_control(active_backend->runtime, be_tostring(vm, 1),
            checked_finite_number(vm, 2), checked_finite_number(vm, 3),
            checked_finite_number(vm, 4),
            (uint16_t)checked_int(vm, 5, 0, UINT16_MAX));
    if (id < 0) native_argument_error(vm, "invalid control definition");
    be_pushint(vm, id); be_return(vm);
}

static int native_control_get(bvm *vm)
{
    require_count(vm, 1);
    bint id = checked_int(vm, 1, 0, SCRIPT_MAX_CONTROLS - 1);
    if ((uint8_t)id >= active_backend->runtime->control_count)
        native_argument_error(vm, "unknown control ID");
    be_pushreal(vm, script_runtime_control_get(active_backend->runtime,
                                                (uint8_t)id));
    be_return(vm);
}

static int native_gate_get(bvm *vm)
{
    require_count(vm, 1);
    bint voice = checked_int(vm, 1, 0, SCRIPT_MAX_VOICES - 1);
    be_pushbool(vm, script_runtime_gate_get(active_backend->runtime,
                                             (uint8_t)voice));
    be_return(vm);
}

static int native_trigger_get(bvm *vm)
{
    require_count(vm, 1);
    bint voice = checked_int(vm, 1, 0, SCRIPT_MAX_VOICES - 1);
    be_pushbool(vm, script_runtime_trigger_get(active_backend->runtime,
                                                (uint8_t)voice));
    be_return(vm);
}

static int native_output_set(bvm *vm)
{
    require_count(vm, 3);
    bint voice = checked_int(vm, 1, 0, SCRIPT_MAX_VOICES - 1);
    bint parameter = checked_int(vm, 2, 0, SCRIPT_MAX_OUTPUTS - 1);
    if (!script_runtime_output_set(active_backend->runtime,
            (uint8_t)voice, (uint8_t)parameter, checked_number(vm, 3))) {
        be_raise(vm, "value_error", "invalid output");
    }
    be_pushnil(vm); be_return(vm);
}

static int native_tick_index(bvm *vm)
{
    require_count(vm, 0);
    be_pushint(vm, (bint)script_runtime_tick_index(active_backend->runtime));
    be_return(vm);
}

static void observation_hook(bvm *vm, int event, ...)
{
    ScriptBerryBackend *b = active_backend;
    (void)vm;
    if (!b) return;
    if (event == BE_OBS_GC_START) {
        ++b->metrics.gc_runs[b->phase];
        if (b->phase == SCRIPT_PHASE_TICK) {
            if (b->pending_fault == SCRIPT_BACKEND_OK)
                b->pending_fault = SCRIPT_BACKEND_GC_IN_TICK;
            b->discard_vm = true;
            if (b->abort_active) longjmp(b->abort_jump, 1);
        }
    } else if (event == BE_OBS_VM_HEARTBEAT &&
               (b->phase == SCRIPT_PHASE_TICK || b->phase == SCRIPT_PHASE_INIT)) {
        uint32_t used = vm->counter_ins - b->tick_instruction_start;
        if (used > b->instruction_budget) {
            b->pending_fault = SCRIPT_BACKEND_WATCHDOG;
            b->discard_vm = true;
            if (b->abort_active) longjmp(b->abort_jump, 1);
        }
    }
}

static uint8_t *backend_begin_upload(void *context, size_t capacity)
{
    ScriptBerryBackend *b = (ScriptBerryBackend *)context;
    if (capacity > SCRIPT_MAX_PAYLOAD) return NULL;
    active_backend = b;
    arena_reset(b, SCRIPT_BERRY_LOAD_HEAP_SIZE);
    b->phase = SCRIPT_PHASE_IDLE;
    return b->arena.bytes + SCRIPT_BERRY_LOAD_HEAP_SIZE;
}

static ScriptBackendResult backend_load(void *context, ScriptRuntime *runtime,
                                        const uint8_t *bytecode, size_t size)
{
    ScriptBerryBackend *b = (ScriptBerryBackend *)context;
    int result, load_result;
    active_backend = b;
    b->runtime = runtime;
    b->phase = SCRIPT_PHASE_INIT;
    b->pending_fault = SCRIPT_BACKEND_OK;
    b->discard_vm = false;
    memory_file.data = bytecode;
    memory_file.size = size;
    memory_file.position = 0;
    b->vm = be_vm_new();
    if (!b->vm) return SCRIPT_BACKEND_OOM;
    be_set_obs_hook((bvm *)b->vm, observation_hook);
    if (((bvm *)b->vm)->stacktop - ((bvm *)b->vm)->stack < BE_STACK_TOTAL_MAX)
        be_stack_expansion((bvm *)b->vm,
            BE_STACK_TOTAL_MAX - (int)(((bvm *)b->vm)->stacktop - ((bvm *)b->vm)->stack));
    be_vector_resize((bvm *)b->vm, &((bvm *)b->vm)->callstack, 16);
    be_vector_clear(&((bvm *)b->vm)->callstack);
    be_regfunc((bvm *)b->vm, "configure_outputs", native_configure_outputs);
    be_regfunc((bvm *)b->vm, "define_control", native_define_control);
    be_regfunc((bvm *)b->vm, "control_get", native_control_get);
    be_regfunc((bvm *)b->vm, "gate_get", native_gate_get);
    be_regfunc((bvm *)b->vm, "trigger_get", native_trigger_get);
    be_regfunc((bvm *)b->vm, "output_set", native_output_set);
    be_regfunc((bvm *)b->vm, "tick_index", native_tick_index);
    load_result = be_loadmode((bvm *)b->vm, "@fwsc-memory", 0);
    result = load_result;
    if (result == BE_OK) {
        b->tick_instruction_start = ((bvm *)b->vm)->counter_ins;
        b->abort_active = true;
        if (setjmp(b->abort_jump) == 0) result = be_pcall((bvm *)b->vm, 0);
        else result = BE_EXCEPTION;
        b->abort_active = false;
    }
    memory_file.data = NULL;
    if (b->pending_fault != SCRIPT_BACKEND_OK) return b->pending_fault;
    if (result != BE_OK) {
        if (result == BE_MALLOC_FAIL) return SCRIPT_BACKEND_OOM;
        return load_result == BE_OK ? SCRIPT_BACKEND_EXCEPTION : SCRIPT_BACKEND_BAD_BYTECODE;
    }
    be_pop((bvm *)b->vm, be_top((bvm *)b->vm));
    if (!be_getglobal((bvm *)b->vm, "tick") || !be_isfunction((bvm *)b->vm, -1)) {
        be_pop((bvm *)b->vm, be_top((bvm *)b->vm));
        return SCRIPT_BACKEND_BAD_ABI;
    }
    be_pop((bvm *)b->vm, be_top((bvm *)b->vm));
    b->phase = SCRIPT_PHASE_IDLE;
    update_arena_metrics(b);
    return SCRIPT_BACKEND_OK;
}

static ScriptBackendResult backend_tick(void *context)
{
    ScriptBerryBackend *b = (ScriptBerryBackend *)context;
    bvm *vm = (bvm *)b->vm;
    int result;
    active_backend = b;
    b->phase = SCRIPT_PHASE_TICK;
    b->pending_fault = SCRIPT_BACKEND_OK;
    b->tick_instruction_start = vm->counter_ins;
    if (!be_getglobal(vm, "tick")) return SCRIPT_BACKEND_BAD_ABI;
    b->abort_active = true;
    if (setjmp(b->abort_jump) == 0) {
        result = be_pcall(vm, 0);
    } else {
        result = BE_EXCEPTION;
    }
    b->abort_active = false;
    b->metrics.instructions_last = vm->counter_ins - b->tick_instruction_start;
    b->metrics.instructions_total += b->metrics.instructions_last;
    if (b->metrics.instructions_last > b->metrics.instructions_max)
        b->metrics.instructions_max = b->metrics.instructions_last;
    if (!b->discard_vm) be_pop(vm, be_top(vm));
    b->phase = SCRIPT_PHASE_IDLE;
    if (b->pending_fault != SCRIPT_BACKEND_OK) return b->pending_fault;
    return result == BE_OK ? SCRIPT_BACKEND_OK : SCRIPT_BACKEND_EXCEPTION;
}

static void backend_stop(void *context)
{
    ScriptBerryBackend *b = (ScriptBerryBackend *)context;
    active_backend = b;
    b->phase = SCRIPT_PHASE_DESTROY;
    if (b->vm && !b->discard_vm) be_vm_delete((bvm *)b->vm);
    b->vm = NULL;
    b->runtime = NULL;
    b->phase = SCRIPT_PHASE_IDLE;
    memory_file.data = NULL;
}

static void backend_metrics(void *context, ScriptBackendMetrics *metrics)
{
    ScriptBerryBackend *b = (ScriptBerryBackend *)context;
    update_arena_metrics(b);
    *metrics = b->metrics;
}

void script_berry_backend_init(ScriptBerryBackend *b, ScriptBackend *backend)
{
    memset(b, 0, sizeof(*b));
    arena_reset(b, SCRIPT_BERRY_LOAD_HEAP_SIZE);
    b->instruction_budget = SCRIPT_BERRY_INSTRUCTION_BUDGET;
    backend->context = b;
    backend->begin_upload = backend_begin_upload;
    backend->load = backend_load;
    backend->tick = backend_tick;
    backend->stop = backend_stop;
    backend->metrics = backend_metrics;
}
