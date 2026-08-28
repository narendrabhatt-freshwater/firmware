#include "script/script_runtime.h"

#include <math.h>
#include <string.h>

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t script_runtime_crc32(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = UINT32_C(0xffffffff);
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static void publish_neutral(ScriptRuntime *r)
{
    memset(r->outputs, 0, sizeof(r->outputs));
    r->published_buffer = 0;
    r->staging_buffer = 1;
    ++r->metrics.output_sequence;
}

static void set_fault(ScriptRuntime *r, ScriptFault fault)
{
    r->fault = fault;
    ++r->metrics.faults;
    r->program_active = false;
    if (r->backend.stop) {
        r->backend.stop(r->backend.context);
    }
    publish_neutral(r);
}

void script_runtime_init(ScriptRuntime *r, const ScriptBackend *backend)
{
    memset(r, 0, sizeof(*r));
    if (backend) {
        r->backend = *backend;
    }
    r->generation = 1;
    publish_neutral(r);
}

void script_runtime_stop(ScriptRuntime *r)
{
    if (r->backend.stop) {
        r->backend.stop(r->backend.context);
    }
    r->program_active = false;
    publish_neutral(r);
}

bool script_runtime_upload_begin(ScriptRuntime *r)
{
    script_runtime_stop(r);
    ++r->generation;
    r->control_count = 0;
    r->output_count = 0;
    r->event_count = 0;
    r->last_sequence = 0;
    r->fault = SCRIPT_FAULT_NONE;
    r->upload_header_bytes = 0;
    r->upload_payload_bytes = 0;
    r->upload_expected_size = 0;
    r->upload_expected_crc = 0;
    r->upload_payload = r->backend.begin_upload
        ? r->backend.begin_upload(r->backend.context, SCRIPT_MAX_PAYLOAD) : NULL;
    r->upload_active = r->upload_payload != NULL;
    if (!r->upload_active) {
        set_fault(r, SCRIPT_FAULT_UPLOAD);
    }
    return r->upload_active;
}

static bool parse_header(ScriptRuntime *r)
{
    const uint8_t *h = r->upload_header;
    if (memcmp(h, "FWSC", 4) != 0 || read_u16(h + 4) != 1u ||
        h[6] != SCRIPT_RUNTIME_ID_BERRY || h[7] != SCRIPT_CONFIG_FLOAT32_INT32 ||
        read_u16(h + 8) != SCRIPT_RUNTIME_ABI_VERSION ||
        read_u16(h + 10) != SCRIPT_FWSC_HEADER_SIZE) {
        return false;
    }
    r->upload_expected_size = read_u32(h + 12);
    r->upload_expected_crc = read_u32(h + 16);
    return r->upload_expected_size <= SCRIPT_MAX_PAYLOAD;
}

bool script_runtime_upload_feed(ScriptRuntime *r, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!r->upload_active || (!p && size)) {
        return false;
    }
    while (size && r->upload_header_bytes < SCRIPT_FWSC_HEADER_SIZE) {
        r->upload_header[r->upload_header_bytes++] = *p++;
        --size;
        if (r->upload_header_bytes == SCRIPT_FWSC_HEADER_SIZE && !parse_header(r)) {
            script_runtime_upload_abort(r);
            set_fault(r, SCRIPT_FAULT_BAD_CONTAINER);
            ++r->metrics.upload_failures;
            return false;
        }
    }
    if (r->upload_header_bytes != SCRIPT_FWSC_HEADER_SIZE) {
        return true;
    }
    if (size > r->upload_expected_size - r->upload_payload_bytes) {
        script_runtime_upload_abort(r);
        set_fault(r, SCRIPT_FAULT_BAD_CONTAINER);
        ++r->metrics.upload_failures;
        return false;
    }
    memcpy(r->upload_payload + r->upload_payload_bytes, p, size);
    r->upload_payload_bytes += (uint32_t)size;
    return true;
}

bool script_runtime_upload_commit(ScriptRuntime *r)
{
    ScriptBackendResult result;
    if (!r->upload_active || r->upload_header_bytes != SCRIPT_FWSC_HEADER_SIZE ||
        r->upload_payload_bytes != r->upload_expected_size ||
        script_runtime_crc32(r->upload_payload, r->upload_payload_bytes) != r->upload_expected_crc) {
        script_runtime_upload_abort(r);
        set_fault(r, SCRIPT_FAULT_BAD_CONTAINER);
        ++r->metrics.upload_failures;
        return false;
    }
    r->initializing = true;
    result = r->backend.load(r->backend.context, r, r->upload_payload,
                             r->upload_payload_bytes);
    r->initializing = false;
    r->upload_active = false;
    if (result != SCRIPT_BACKEND_OK || r->output_count == 0) {
        ScriptFault fault = SCRIPT_FAULT_EXCEPTION;
        if (result == SCRIPT_BACKEND_BAD_BYTECODE) fault = SCRIPT_FAULT_BAD_BYTECODE;
        else if (result == SCRIPT_BACKEND_BAD_ABI || r->output_count == 0)
            fault = SCRIPT_FAULT_NOT_CONFIGURED;
        else if (result == SCRIPT_BACKEND_OOM) fault = SCRIPT_FAULT_UPLOAD;
        else if (result == SCRIPT_BACKEND_WATCHDOG) fault = SCRIPT_FAULT_WATCHDOG;
        else if (result == SCRIPT_BACKEND_ALLOCATION_IN_TICK) fault = SCRIPT_FAULT_ALLOCATION;
        else if (result == SCRIPT_BACKEND_GC_IN_TICK) fault = SCRIPT_FAULT_GC;
        set_fault(r, fault);
        ++r->metrics.upload_failures;
        return false;
    }
    r->program_active = true;
    r->fault = SCRIPT_FAULT_NONE;
    return true;
}

void script_runtime_upload_abort(ScriptRuntime *r)
{
    r->upload_active = false;
    r->upload_header_bytes = 0;
    r->upload_payload_bytes = 0;
    r->program_active = false;
    publish_neutral(r);
}

static bool enqueue(ScriptRuntime *r, const ScriptEvent *event)
{
    if (event->generation != r->generation) {
        ++r->metrics.stale_generation;
        return false;
    }
    if (event->sequence <= r->last_sequence) {
        ++r->metrics.stale_sequence;
        return false;
    }
    if (r->event_count == SCRIPT_EVENT_CAPACITY) {
        ++r->metrics.queue_overflow;
        return false;
    }
    r->events[r->event_count++] = *event;
    r->last_sequence = event->sequence;
    ++r->metrics.queue_accepted;
    return true;
}

bool script_runtime_enqueue_control(ScriptRuntime *r, uint32_t generation,
                                    uint32_t sequence, uint8_t id, float value,
                                    uint32_t apply_tick, uint16_t ramp_ticks)
{
    ScriptEvent e = {generation, sequence, apply_tick, ramp_ticks,
                     SCRIPT_EVENT_CONTROL, id, value};
    if (id >= r->control_count || !isfinite(value)) {
        return false;
    }
    return enqueue(r, &e);
}

bool script_runtime_enqueue_gate(ScriptRuntime *r, uint32_t generation,
                                 uint32_t sequence, uint8_t voice, bool state,
                                 uint32_t apply_tick)
{
    ScriptEvent e = {generation, sequence, apply_tick, 0,
                     SCRIPT_EVENT_GATE, voice, state ? 1.0f : 0.0f};
    return voice < SCRIPT_MAX_VOICES && enqueue(r, &e);
}

bool script_runtime_enqueue_trigger(ScriptRuntime *r, uint32_t generation,
                                    uint32_t sequence, uint8_t voice,
                                    uint32_t apply_tick)
{
    ScriptEvent e = {generation, sequence, apply_tick, 0,
                     SCRIPT_EVENT_TRIGGER, voice, 1.0f};
    return voice < SCRIPT_MAX_VOICES && enqueue(r, &e);
}

static void apply_events(ScriptRuntime *r)
{
    uint8_t dst = 0;
    memset(r->triggers, 0, sizeof(r->triggers));
    for (uint8_t i = 0; i < r->event_count; ++i) {
        ScriptEvent *e = &r->events[i];
        if (e->apply_tick > r->tick_index) {
            r->events[dst++] = *e;
            continue;
        }
        if (e->type == SCRIPT_EVENT_CONTROL) {
            ScriptControl *c = &r->controls[e->id];
            float target = e->value;
            if (target < c->minimum) { target = c->minimum; ++r->metrics.range_clamps; }
            if (target > c->maximum) { target = c->maximum; ++r->metrics.range_clamps; }
            c->target = target;
            c->ramp_remaining = e->ramp_ticks ? e->ramp_ticks : c->slew_ms;
            if (c->ramp_remaining) {
                c->step = (target - c->current) / (float)c->ramp_remaining;
            } else {
                c->current = target;
                c->step = 0.0f;
            }
        } else if (e->type == SCRIPT_EVENT_GATE) {
            r->gates[e->id] = e->value != 0.0f;
        } else {
            r->triggers[e->id] = 1;
        }
        ++r->metrics.queue_applied;
    }
    r->event_count = dst;
}

static void snapshot_controls(ScriptRuntime *r)
{
    for (uint8_t i = 0; i < r->control_count; ++i) {
        ScriptControl *c = &r->controls[i];
        if (c->ramp_remaining) {
            c->current += c->step;
            if (--c->ramp_remaining == 0) {
                c->current = c->target;
            }
        }
        r->control_snapshot[i] = c->current;
    }
}

bool script_runtime_tick(ScriptRuntime *r)
{
    ScriptBackendResult result;
    uint16_t required;
    if (!r->program_active) {
        return false;
    }
    apply_events(r);
    snapshot_controls(r);
    memset(r->outputs[r->staging_buffer], 0, sizeof(r->outputs[0]));
    memset(r->written_mask, 0, sizeof(r->written_mask));
    result = r->backend.tick(r->backend.context);
    if (r->fault != SCRIPT_FAULT_NONE) {
        ScriptFault fault = r->fault;
        set_fault(r, fault);
        return false;
    }
    if (result != SCRIPT_BACKEND_OK) {
        ScriptFault f = SCRIPT_FAULT_EXCEPTION;
        if (result == SCRIPT_BACKEND_WATCHDOG) f = SCRIPT_FAULT_WATCHDOG;
        if (result == SCRIPT_BACKEND_ALLOCATION_IN_TICK) f = SCRIPT_FAULT_ALLOCATION;
        if (result == SCRIPT_BACKEND_GC_IN_TICK) f = SCRIPT_FAULT_GC;
        set_fault(r, f);
        return false;
    }
    required = (uint16_t)((1u << r->output_count) - 1u);
    for (uint8_t voice = 0; voice < SCRIPT_MAX_VOICES; ++voice) {
        if (r->written_mask[voice] != required) {
            set_fault(r, SCRIPT_FAULT_MISSING_OUTPUT);
            return false;
        }
    }
    r->published_buffer = r->staging_buffer;
    r->staging_buffer ^= 1u;
    ++r->metrics.output_sequence;
    ++r->metrics.ticks_completed;
    r->metrics.outputs_generated += SCRIPT_MAX_VOICES * r->output_count;
    ++r->tick_index;
    return true;
}

int script_runtime_configure_outputs(ScriptRuntime *r, uint8_t count)
{
    if (!r->initializing || r->output_count || count < 1 || count > SCRIPT_MAX_OUTPUTS) {
        return -1;
    }
    r->output_count = count;
    return 0;
}

int script_runtime_define_control(ScriptRuntime *r, const char *name,
                                  float minimum, float maximum, float value,
                                  uint16_t slew_ms)
{
    ScriptControl *c;
    size_t length;
    if (!r->initializing || !name || !isfinite(minimum) || !isfinite(maximum) ||
        !isfinite(value) || minimum > maximum || value < minimum || value > maximum ||
        r->control_count == SCRIPT_MAX_CONTROLS || script_runtime_find_control(r, name) >= 0) {
        return -1;
    }
    length = strlen(name);
    if (length == 0 || length >= SCRIPT_CONTROL_NAME_MAX) {
        return -1;
    }
    c = &r->controls[r->control_count];
    memset(c, 0, sizeof(*c));
    memcpy(c->name, name, length + 1);
    c->minimum = minimum;
    c->maximum = maximum;
    c->current = c->target = value;
    c->slew_ms = slew_ms;
    return r->control_count++;
}

float script_runtime_control_get(const ScriptRuntime *r, uint8_t id)
{
    return id < r->control_count ? r->control_snapshot[id] : 0.0f;
}

bool script_runtime_gate_get(const ScriptRuntime *r, uint8_t voice)
{
    return voice < SCRIPT_MAX_VOICES && r->gates[voice];
}

bool script_runtime_trigger_get(const ScriptRuntime *r, uint8_t voice)
{
    return voice < SCRIPT_MAX_VOICES && r->triggers[voice];
}

bool script_runtime_output_set(ScriptRuntime *r, uint8_t voice, uint8_t parameter,
                               float value)
{
    if (voice >= SCRIPT_MAX_VOICES || parameter >= r->output_count) {
        r->fault = SCRIPT_FAULT_BAD_NATIVE_ARGUMENT;
        return false;
    }
    if (!isfinite(value)) {
        r->fault = SCRIPT_FAULT_NAN_OR_INF;
        return false;
    }
    if (value < -1.0f) { value = -1.0f; ++r->metrics.clamps; }
    if (value > 1.0f) { value = 1.0f; ++r->metrics.clamps; }
    r->outputs[r->staging_buffer][voice][parameter] = value;
    r->written_mask[voice] |= (uint16_t)(1u << parameter);
    return true;
}

uint32_t script_runtime_tick_index(const ScriptRuntime *r) { return r->tick_index; }

const float (*script_runtime_outputs(const ScriptRuntime *r))[SCRIPT_MAX_OUTPUTS]
{
    return r->outputs[r->published_buffer];
}

int script_runtime_find_control(const ScriptRuntime *r, const char *name)
{
    if (!name) return -1;
    for (uint8_t i = 0; i < r->control_count; ++i) {
        if (strcmp(r->controls[i].name, name) == 0) return i;
    }
    return -1;
}
