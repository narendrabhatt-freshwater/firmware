#ifndef SCRIPT_RUNTIME_H
#define SCRIPT_RUNTIME_H

#include "script_backend.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_MAX_VOICES 8u
#define SCRIPT_MAX_OUTPUTS 10u
#define SCRIPT_MAX_CONTROLS 32u
#define SCRIPT_EVENT_CAPACITY 32u
#define SCRIPT_MAX_PAYLOAD 4096u
#define SCRIPT_CONTROL_NAME_MAX 20u
#define SCRIPT_FWSC_HEADER_SIZE 20u
#define SCRIPT_RUNTIME_ABI_VERSION 1u
#define SCRIPT_RUNTIME_ID_BERRY 1u
#define SCRIPT_CONFIG_FLOAT32_INT32 0x03u

typedef enum {
    SCRIPT_FAULT_NONE = 0,
    SCRIPT_FAULT_UPLOAD,
    SCRIPT_FAULT_BAD_CONTAINER,
    SCRIPT_FAULT_BAD_BYTECODE,
    SCRIPT_FAULT_EXCEPTION,
    SCRIPT_FAULT_WATCHDOG,
    SCRIPT_FAULT_ALLOCATION,
    SCRIPT_FAULT_GC,
    SCRIPT_FAULT_NAN_OR_INF,
    SCRIPT_FAULT_MISSING_OUTPUT,
    SCRIPT_FAULT_BAD_NATIVE_ARGUMENT,
    SCRIPT_FAULT_NOT_CONFIGURED
} ScriptFault;

typedef enum {
    SCRIPT_EVENT_CONTROL = 0,
    SCRIPT_EVENT_GATE,
    SCRIPT_EVENT_TRIGGER
} ScriptEventType;

typedef struct {
    char name[SCRIPT_CONTROL_NAME_MAX];
    float minimum;
    float maximum;
    float current;
    float target;
    float step;
    uint32_t ramp_remaining;
    uint16_t slew_ms;
} ScriptControl;

typedef struct {
    uint32_t generation;
    uint32_t sequence;
    uint32_t apply_tick;
    uint16_t ramp_ticks;
    uint8_t type;
    uint8_t id;
    float value;
} ScriptEvent;

typedef struct {
    uint32_t ticks_completed;
    uint64_t outputs_generated;
    uint32_t clamps;
    uint32_t faults;
    uint32_t queue_accepted;
    uint32_t queue_applied;
    uint32_t queue_overflow;
    uint32_t stale_generation;
    uint32_t stale_sequence;
    uint32_t range_clamps;
    uint32_t upload_failures;
    uint32_t output_sequence;
} ScriptRuntimeMetrics;

typedef struct ScriptRuntime {
    ScriptBackend backend;
    ScriptControl controls[SCRIPT_MAX_CONTROLS];
    ScriptEvent events[SCRIPT_EVENT_CAPACITY];
    float control_snapshot[SCRIPT_MAX_CONTROLS];
    float outputs[2][SCRIPT_MAX_VOICES][SCRIPT_MAX_OUTPUTS];
    uint16_t written_mask[SCRIPT_MAX_VOICES];
    uint8_t gates[SCRIPT_MAX_VOICES];
    uint8_t triggers[SCRIPT_MAX_VOICES];
    uint8_t control_count;
    uint8_t output_count;
    uint8_t event_count;
    uint8_t published_buffer;
    uint8_t staging_buffer;
    bool program_active;
    bool initializing;
    uint32_t generation;
    uint32_t last_sequence;
    uint32_t tick_index;
    ScriptFault fault;
    ScriptRuntimeMetrics metrics;
    uint8_t upload_header[SCRIPT_FWSC_HEADER_SIZE];
    uint8_t upload_header_bytes;
    uint32_t upload_payload_bytes;
    uint32_t upload_expected_size;
    uint32_t upload_expected_crc;
    uint8_t *upload_payload;
    bool upload_active;
} ScriptRuntime;

void script_runtime_init(ScriptRuntime *runtime, const ScriptBackend *backend);
void script_runtime_stop(ScriptRuntime *runtime);
bool script_runtime_upload_begin(ScriptRuntime *runtime);
bool script_runtime_upload_feed(ScriptRuntime *runtime, const void *data, size_t size);
bool script_runtime_upload_commit(ScriptRuntime *runtime);
void script_runtime_upload_abort(ScriptRuntime *runtime);

bool script_runtime_enqueue_control(ScriptRuntime *runtime, uint32_t generation,
                                    uint32_t sequence, uint8_t control_id,
                                    float value, uint32_t apply_tick,
                                    uint16_t ramp_ticks);
bool script_runtime_enqueue_gate(ScriptRuntime *runtime, uint32_t generation,
                                 uint32_t sequence, uint8_t voice, bool state,
                                 uint32_t apply_tick);
bool script_runtime_enqueue_trigger(ScriptRuntime *runtime, uint32_t generation,
                                    uint32_t sequence, uint8_t voice,
                                    uint32_t apply_tick);
bool script_runtime_tick(ScriptRuntime *runtime);

int script_runtime_configure_outputs(ScriptRuntime *runtime, uint8_t count);
int script_runtime_define_control(ScriptRuntime *runtime, const char *name,
                                  float minimum, float maximum, float default_value,
                                  uint16_t slew_ms);
float script_runtime_control_get(const ScriptRuntime *runtime, uint8_t id);
bool script_runtime_gate_get(const ScriptRuntime *runtime, uint8_t voice);
bool script_runtime_trigger_get(const ScriptRuntime *runtime, uint8_t voice);
bool script_runtime_output_set(ScriptRuntime *runtime, uint8_t voice,
                               uint8_t parameter, float value);
uint32_t script_runtime_tick_index(const ScriptRuntime *runtime);
const float (*script_runtime_outputs(const ScriptRuntime *runtime))[SCRIPT_MAX_OUTPUTS];
int script_runtime_find_control(const ScriptRuntime *runtime, const char *name);
uint32_t script_runtime_crc32(const void *data, size_t size);

#endif
