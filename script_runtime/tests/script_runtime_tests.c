#include "script/script_runtime.h"
#include "script/berry_backend.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t upload[SCRIPT_MAX_PAYLOAD];
    ScriptRuntime *runtime;
    bool omit_output;
    bool nan_output;
    bool clamp_output;
    bool observed_note_on;
    bool observed_note_started;
} MockBackend;

static uint8_t *mock_begin(void *context, size_t capacity)
{
    MockBackend *mock = (MockBackend *)context;
    return capacity <= sizeof(mock->upload) ? mock->upload : NULL;
}

static ScriptBackendResult mock_load(void *context, ScriptRuntime *runtime,
                                     const uint8_t *data, size_t size)
{
    MockBackend *mock = (MockBackend *)context;
    (void)data;
    if (size == 0) return SCRIPT_BACKEND_BAD_BYTECODE;
    mock->runtime = runtime;
    assert(script_runtime_configure_outputs(runtime, 3) == 0);
    assert(script_runtime_define_control(runtime, "rate", 0.0f, 1.0f, 0.0f, 0) == 0);
    return SCRIPT_BACKEND_OK;
}

static ScriptBackendResult mock_tick(void *context)
{
    MockBackend *mock = (MockBackend *)context;
    mock->observed_note_on = script_runtime_note_is_on(mock->runtime, 3);
    mock->observed_note_started = script_runtime_note_started(mock->runtime, 3);
    for (uint8_t note = 0; note < SCRIPT_MAX_NOTE_SLOTS; ++note) {
        for (uint8_t output = 0; output < 3; ++output) {
            if (mock->omit_output && note == 7 && output == 2) continue;
            float value = script_runtime_control_get(mock->runtime, 0);
            if (mock->nan_output && note == 0 && output == 0) value = NAN;
            if (mock->clamp_output) value = 2.0f;
            script_runtime_output_set(mock->runtime, note, output, value);
        }
    }
    return SCRIPT_BACKEND_OK;
}

static void mock_stop(void *context) { (void)context; }
static void mock_metrics(void *context, ScriptBackendMetrics *metrics)
{ (void)context; memset(metrics, 0, sizeof(*metrics)); }

static void put_u16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static size_t make_container(uint8_t *container)
{
    const uint8_t payload[4] = {1, 2, 3, 4};
    memset(container, 0, 24);
    memcpy(container, "FWSC", 4);
    put_u16(container + 4, 1);
    container[6] = SCRIPT_RUNTIME_ID_BERRY;
    container[7] = SCRIPT_CONFIG_FLOAT32_INT32;
    put_u16(container + 8, SCRIPT_RUNTIME_ABI_VERSION);
    put_u16(container + 10, SCRIPT_FWSC_HEADER_SIZE);
    put_u32(container + 12, sizeof(payload));
    put_u32(container + 16, script_runtime_crc32(payload, sizeof(payload)));
    memcpy(container + 20, payload, sizeof(payload));
    return 24;
}

static ScriptBackend backend_for(MockBackend *mock)
{
    ScriptBackend backend = {mock, mock_begin, mock_load, mock_tick, mock_stop, mock_metrics};
    return backend;
}

static void upload_one_byte_chunks(ScriptRuntime *runtime, const uint8_t *data, size_t size)
{
    assert(script_runtime_upload_begin(runtime));
    for (size_t i = 0; i < size; ++i) assert(script_runtime_upload_feed(runtime, data + i, 1));
    assert(script_runtime_upload_commit(runtime));
}

static void test_upload_tick_and_ramp(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    upload_one_byte_chunks(&runtime, container, size);
    assert(runtime.output_count == 3);
    assert(script_runtime_enqueue_control(&runtime, runtime.generation, 1, 0, 1.5f, 2, 2));
    assert(script_runtime_tick(&runtime));
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
    assert(script_runtime_tick(&runtime));
    assert(script_runtime_tick(&runtime));
    assert(fabsf(script_runtime_outputs(&runtime)[0][0] - 0.5f) < 1e-6f);
    assert(script_runtime_tick(&runtime));
    assert(script_runtime_outputs(&runtime)[0][0] == 1.0f);
    assert(runtime.metrics.range_clamps == 1);
    assert(runtime.metrics.outputs_generated == 96);
}

static void test_queue_policy(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    upload_one_byte_chunks(&runtime, container, size);
    assert(!script_runtime_enqueue_control(&runtime, runtime.generation - 1, 1, 0, 0, 99, 0));
    for (uint32_t i = 1; i <= SCRIPT_EVENT_CAPACITY; ++i)
        assert(script_runtime_enqueue_control(&runtime, runtime.generation, i, 0, 0, 99, 0));
    assert(!script_runtime_enqueue_control(&runtime, runtime.generation,
                                            SCRIPT_EVENT_CAPACITY + 1, 0, 0, 99, 0));
    assert(runtime.metrics.queue_overflow == 1);
    assert(!script_runtime_enqueue_control(&runtime, runtime.generation,
                                            SCRIPT_EVENT_CAPACITY, 0, 0, 99, 0));
    assert(runtime.metrics.stale_sequence == 1);
}

static void test_note_event_lifetime(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    upload_one_byte_chunks(&runtime, container, size);
    assert(script_runtime_enqueue_note_on(&runtime, runtime.generation, 1, 3, 0));
    assert(script_runtime_tick(&runtime));
    assert(mock.observed_note_on && mock.observed_note_started);
    assert(script_runtime_tick(&runtime));
    assert(mock.observed_note_on && !mock.observed_note_started);
    assert(script_runtime_enqueue_note_restart(&runtime, runtime.generation,
                                               2, 3, runtime.tick_index));
    assert(script_runtime_tick(&runtime));
    assert(mock.observed_note_on && mock.observed_note_started);
    assert(script_runtime_enqueue_note_off(&runtime, runtime.generation,
                                           3, 3, runtime.tick_index));
    assert(script_runtime_tick(&runtime));
    assert(!mock.observed_note_on && !mock.observed_note_started);
}

static void test_maximum_upload_with_varied_chunks(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[SCRIPT_FWSC_HEADER_SIZE + SCRIPT_MAX_PAYLOAD] = {0};
    size_t offset = 0;
    memcpy(container, "FWSC", 4);
    put_u16(container + 4, 1);
    container[6] = SCRIPT_RUNTIME_ID_BERRY;
    container[7] = SCRIPT_CONFIG_FLOAT32_INT32;
    put_u16(container + 8, SCRIPT_RUNTIME_ABI_VERSION);
    put_u16(container + 10, SCRIPT_FWSC_HEADER_SIZE);
    put_u32(container + 12, SCRIPT_MAX_PAYLOAD);
    for (size_t i = 0; i < SCRIPT_MAX_PAYLOAD; ++i)
        container[SCRIPT_FWSC_HEADER_SIZE + i] = (uint8_t)(i * 37u + 11u);
    put_u32(container + 16, script_runtime_crc32(container + SCRIPT_FWSC_HEADER_SIZE,
                                                 SCRIPT_MAX_PAYLOAD));
    script_runtime_init(&runtime, &backend);
    assert(script_runtime_upload_begin(&runtime));
    while (offset < sizeof(container)) {
        size_t chunk = (offset * 17u + 1u) % 97u + 1u;
        if (chunk > sizeof(container) - offset) chunk = sizeof(container) - offset;
        assert(script_runtime_upload_feed(&runtime, container + offset, chunk));
        offset += chunk;
    }
    assert(script_runtime_upload_commit(&runtime));
    script_runtime_upload_abort(&runtime);
    assert(!runtime.program_active);
}

static void test_faults_are_neutral(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    upload_one_byte_chunks(&runtime, container, size);
    mock.omit_output = true;
    assert(!script_runtime_tick(&runtime));
    assert(runtime.fault == SCRIPT_FAULT_MISSING_OUTPUT);
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);

    upload_one_byte_chunks(&runtime, container, size);
    mock.omit_output = false; mock.nan_output = true;
    assert(!script_runtime_tick(&runtime));
    assert(runtime.fault == SCRIPT_FAULT_NAN_OR_INF);
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
}

static void test_output_clamping(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    upload_one_byte_chunks(&runtime, container, size);
    mock.clamp_output = true;
    assert(script_runtime_tick(&runtime));
    assert(script_runtime_outputs(&runtime)[7][2] == 1.0f);
    assert(runtime.metrics.clamps == SCRIPT_MAX_NOTE_SLOTS * 3u);
}

static void expect_header_rejected(ScriptRuntime *runtime, const uint8_t *container)
{
    assert(script_runtime_upload_begin(runtime));
    assert(!script_runtime_upload_feed(runtime, container, SCRIPT_FWSC_HEADER_SIZE));
    assert(runtime->fault == SCRIPT_FAULT_BAD_CONTAINER);
    assert(!runtime->program_active);
    assert(script_runtime_outputs(runtime)[0][0] == 0.0f);
}

static void test_bad_containers(void)
{
    MockBackend mock = {0};
    ScriptBackend backend = backend_for(&mock);
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_runtime_init(&runtime, &backend);
    container[23] ^= 1;
    assert(script_runtime_upload_begin(&runtime));
    assert(script_runtime_upload_feed(&runtime, container, size));
    assert(!script_runtime_upload_commit(&runtime));
    assert(!runtime.program_active);
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
    make_container(container);
    put_u16(container + 8, 1u);
    assert(script_runtime_upload_begin(&runtime));
    assert(!script_runtime_upload_feed(&runtime, container, size));
    assert(!runtime.program_active);

    make_container(container);
    put_u32(container + 12, SCRIPT_MAX_PAYLOAD + 1u);
    assert(script_runtime_upload_begin(&runtime));
    assert(!script_runtime_upload_feed(&runtime, container, SCRIPT_FWSC_HEADER_SIZE));
    assert(runtime.fault == SCRIPT_FAULT_BAD_CONTAINER);

    make_container(container);
    assert(script_runtime_upload_begin(&runtime));
    assert(script_runtime_upload_feed(&runtime, container, 7));
    assert(!script_runtime_upload_commit(&runtime));
    assert(runtime.fault == SCRIPT_FAULT_BAD_CONTAINER);

    make_container(container);
    assert(script_runtime_upload_begin(&runtime));
    assert(script_runtime_upload_feed(&runtime, container, size - 1));
    assert(!script_runtime_upload_commit(&runtime));
    assert(runtime.fault == SCRIPT_FAULT_BAD_CONTAINER);

    make_container(container);
    assert(script_runtime_upload_begin(&runtime));
    assert(script_runtime_upload_feed(&runtime, container, size));
    assert(!script_runtime_upload_feed(&runtime, container, 1));
    assert(runtime.fault == SCRIPT_FAULT_BAD_CONTAINER);

    make_container(container); container[0] = 'X';
    expect_header_rejected(&runtime, container);
    make_container(container); container[4] = 2;
    expect_header_rejected(&runtime, container);
    make_container(container); container[6] = 2;
    expect_header_rejected(&runtime, container);
    make_container(container); container[7] = 0;
    expect_header_rejected(&runtime, container);
    make_container(container);
    put_u16(container + 8, SCRIPT_RUNTIME_ABI_VERSION + 1u);
    expect_header_rejected(&runtime, container);
    make_container(container); container[10] = 19;
    expect_header_rejected(&runtime, container);

    make_container(container);
    upload_one_byte_chunks(&runtime, container, size);
    assert(script_runtime_tick(&runtime));
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
    assert(script_runtime_upload_begin(&runtime));
    assert(!runtime.program_active);
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
    assert(script_runtime_upload_feed(&runtime, container, 5));
    script_runtime_upload_abort(&runtime);
    assert(!runtime.program_active);
    assert(script_runtime_outputs(&runtime)[0][0] == 0.0f);
}

static void test_berry_rejects_invalid_bytecode(void)
{
    ScriptBerryBackend berry;
    ScriptBackend backend;
    ScriptRuntime runtime;
    uint8_t container[24];
    size_t size = make_container(container);
    script_berry_backend_init(&berry, &backend);
    script_runtime_init(&runtime, &backend);
    assert(script_runtime_upload_begin(&runtime));
    assert(script_runtime_upload_feed(&runtime, container, size));
    assert(!script_runtime_upload_commit(&runtime));
    assert(runtime.fault == SCRIPT_FAULT_BAD_BYTECODE);
    assert(!runtime.program_active);
}

int main(void)
{
    test_upload_tick_and_ramp();
    test_queue_policy();
    test_note_event_lifetime();
    test_maximum_upload_with_varied_chunks();
    test_faults_are_neutral();
    test_output_clamping();
    test_bad_containers();
    test_berry_rejects_invalid_bytecode();
    puts("script_runtime_tests: ok");
    return 0;
}
