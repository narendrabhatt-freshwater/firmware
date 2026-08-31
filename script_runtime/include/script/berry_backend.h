#ifndef SCRIPT_BERRY_BACKEND_H
#define SCRIPT_BERRY_BACKEND_H

#include "script/script_runtime.h"
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ABI6 eight-program/reload peak 17,120 B + 4 KiB upload scratch + 20% headroom,
 * rounded to 1 KiB by the qualification probe. */
#define SCRIPT_BERRY_ARENA_SIZE (25u * 1024u)
#define SCRIPT_BERRY_UPLOAD_SIZE FW_SCRIPT_MAX_PAYLOAD
#define SCRIPT_BERRY_HEAP_SIZE (SCRIPT_BERRY_ARENA_SIZE-SCRIPT_BERRY_UPLOAD_SIZE)
/* Largest sanctioned handler path measured 36 instructions. Add 25%, then
 * round up to the 32-instruction observation quantum. */
#define SCRIPT_BERRY_SANCTIONED_HANDLER_MAX 36u
#define SCRIPT_BERRY_HANDLER_INSTRUCTION_LIMIT 64u
#define SCRIPT_BERRY_BOUNDARY_INSTRUCTION_LIMIT \
  (FW_SCRIPT_CHANNEL_VOICE_COUNT * SCRIPT_BERRY_HANDLER_INSTRUCTION_LIMIT)
#define SCRIPT_BERRY_BOUNDARY_CYCLE_LIMIT 55000u

typedef struct {
  void *context;
  int (*read_input)(void *, uint8_t, FwVmChannelInput, float *);
  int (*set_amplitude)(void *, uint8_t, float);
  int (*ramp)(void *, uint8_t, float, float);
  int (*hold)(void *, uint8_t);
  int (*start_note)(void *, uint8_t);
  int (*note_end)(void *, uint8_t);
  void (*silence_voice)(void *, uint8_t, FwVmFault);
  int (*set_led)(void *, uint8_t, float, float, float, float);
  int (*discard_pending)(void *, uint8_t);
} ScriptBerryNativeOps;

typedef union {
  long double alignment;
  uint8_t bytes[SCRIPT_BERRY_HEAP_SIZE];
} ScriptBerryArena;

typedef struct ScriptBerryRuntime {
  ScriptBerryArena arena;
  void *vm;
  ScriptBerryNativeOps ops;
  float state[FW_SCRIPT_CHANNEL_VOICE_COUNT][FW_SCRIPT_CHANNEL_STATE_VALUES];
  float tuning_scale[FW_SCRIPT_CHANNEL_VOICE_COUNT];
  FwVmMetrics voice_metrics[FW_SCRIPT_CHANNEL_VOICE_COUNT];
  FwVmFault voice_fault[FW_SCRIPT_CHANNEL_VOICE_COUNT];
  FwVmMemoryMetrics memory;
  uint8_t active_mask;
  uint8_t current_voice;
  uint8_t phase;
  uint8_t shared_valid;
  uint8_t discard_vm;
  uint8_t abort_active;
  uint8_t upload_active;
  uint8_t upload_voice;
  uint8_t upload_header_bytes;
  uint8_t upload_header[FW_SCRIPT_CONTAINER_HEADER_SIZE];
  uint32_t upload_payload_bytes;
  uint32_t upload_expected_size;
  uint32_t upload_expected_crc;
  uint32_t handler_instruction_start;
  uint32_t boundary_instructions;
  uint32_t boundary_cycles;
  uint32_t heap_limit;
  uint32_t allocations[4];
  uint32_t frees[4];
  uint32_t gc_runs[4];
  FwVmFault pending_fault;
  jmp_buf abort_jump;
} ScriptBerryRuntime;

void script_berry_init(ScriptBerryRuntime *, const ScriptBerryNativeOps *);
void script_berry_stop(ScriptBerryRuntime *, uint8_t voice);
void script_berry_stop_all(ScriptBerryRuntime *);
uint8_t script_berry_is_active(const ScriptBerryRuntime *, uint8_t voice);
uint8_t script_berry_active_mask(const ScriptBerryRuntime *);
uint8_t script_berry_map_key(const ScriptBerryRuntime *, uint8_t voice, uint8_t key);
float script_berry_tuning_scale(const ScriptBerryRuntime *, uint8_t voice);
FwVmFault script_berry_fault(const ScriptBerryRuntime *, uint8_t voice);
const FwVmMetrics *script_berry_voice_metrics(const ScriptBerryRuntime *, uint8_t);
const FwVmMemoryMetrics *script_berry_memory_metrics(ScriptBerryRuntime *);
void script_berry_boundary_begin(ScriptBerryRuntime *);
void script_berry_record_cycles(ScriptBerryRuntime *, uint8_t, uint32_t);
int script_berry_dispatch(ScriptBerryRuntime *, FwVmChannelHandler, uint8_t);
int script_berry_upload_begin(ScriptBerryRuntime *, uint8_t voice);
int script_berry_upload_feed(ScriptBerryRuntime *, const void *, size_t);
int script_berry_upload_commit(ScriptBerryRuntime *);
void script_berry_upload_abort(ScriptBerryRuntime *);
uint8_t script_berry_upload_is_active(const ScriptBerryRuntime *, uint8_t);

/* Berry port hooks selected by berry_conf.h. */
void *script_berry_malloc(size_t);
void script_berry_free(void *);
void *script_berry_realloc(void *, size_t);

#endif
