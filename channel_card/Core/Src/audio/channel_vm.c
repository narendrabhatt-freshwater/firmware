#include "channel_vm.h"
#include "script/berry_backend.h"
#include <string.h>

static ScriptBerryRuntime s_runtime
#if defined(__arm__) || defined(__thumb__)
    __attribute__((aligned(8), section(".vm_arena")))
#endif
    ;

void ChannelVm_Init(const ChannelVmNativeOps *ops)
{
  ScriptBerryNativeOps native_ops;
  memset(&native_ops, 0, sizeof(native_ops));
  if (ops != NULL) {
    native_ops.context = ops->context;
    native_ops.read_input = ops->read_input;
    native_ops.set_amplitude = ops->set_amplitude;
    native_ops.ramp = ops->ramp;
    native_ops.start_note = ops->start_note;
    native_ops.note_end = ops->note_end;
    native_ops.discard_pending = ops->discard_pending;
    native_ops.start_note_at = ops->start_note_at;
    native_ops.osc = ops->osc;
    native_ops.route = ops->route;
    native_ops.silence_voice = ops->silence_voice;
    native_ops.set_led = ops->set_led;
  }
  script_berry_init(&s_runtime, &native_ops);
}
void ChannelVm_Stop(uint8_t voice) { script_berry_stop(&s_runtime, voice); }
void ChannelVm_StopAll(void) { script_berry_stop_all(&s_runtime); }
uint8_t ChannelVm_IsActive(uint8_t voice) { return script_berry_is_active(&s_runtime, voice); }
uint8_t ChannelVm_ActiveMask(void) { return script_berry_active_mask(&s_runtime); }
FwVmFault ChannelVm_Fault(uint8_t voice) { return script_berry_fault(&s_runtime, voice); }
const FwVmMetrics *ChannelVm_Metrics(uint8_t voice) { return script_berry_voice_metrics(&s_runtime, voice); }
const FwVmMemoryMetrics *ChannelVm_MemoryMetrics(void) { return script_berry_memory_metrics(&s_runtime); }
void ChannelVm_BoundaryBegin(void) { script_berry_boundary_begin(&s_runtime); }
void ChannelVm_RecordCycles(uint8_t voice, uint32_t cycles) { script_berry_record_cycles(&s_runtime, voice, cycles); }
int ChannelVm_Dispatch(FwVmChannelHandler handler, uint8_t voice) { return script_berry_dispatch(&s_runtime, handler, voice); }
int ChannelVm_UploadBegin(uint8_t voice) { return script_berry_upload_begin(&s_runtime, voice); }
int ChannelVm_UploadFeed(uint8_t voice, const void *data, size_t size) {
  return script_berry_upload_is_active(&s_runtime, voice) ? script_berry_upload_feed(&s_runtime, data, size) : -1;
}
int ChannelVm_UploadCommit(uint8_t voice) {
  return script_berry_upload_is_active(&s_runtime, voice) ? script_berry_upload_commit(&s_runtime) : -1;
}
void ChannelVm_UploadAbort(uint8_t voice) {
  if (script_berry_upload_is_active(&s_runtime, voice)) script_berry_upload_abort(&s_runtime);
}
uint8_t ChannelVm_UploadIsActive(uint8_t voice) { return script_berry_upload_is_active(&s_runtime, voice); }
uint8_t ChannelVm_UploadIsBusy(void) { return s_runtime.upload_active; }
