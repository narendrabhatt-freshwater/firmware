#ifndef CHANNEL_VM_H
#define CHANNEL_VM_H

#include "freshwater/vm.h"
#include "freshwater/vm_channel.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  void *context;
  int (*read_input)(void *context, uint8_t note, FwVmChannelInput input,
                    float *value);
  int (*set_amplitude)(void *context, uint8_t note, float amplitude);
  int (*ramp)(void *context, uint8_t note, float target, float slope);
  int (*hold)(void *context, uint8_t note);
  int (*start_note)(void *context, uint8_t note);
  int (*note_end)(void *context, uint8_t note);
  void (*silence_voice)(void *context, uint8_t voice, FwVmFault fault);
  int (*set_led)(void *context, uint8_t voice, float red, float green,
                 float blue, float brightness);
  int (*discard_pending)(void *context, uint8_t note);
} ChannelVmNativeOps;

void ChannelVm_Init(const ChannelVmNativeOps *ops);
void ChannelVm_Stop(uint8_t voice);
void ChannelVm_StopAll(void);
uint8_t ChannelVm_IsActive(uint8_t voice);
uint8_t ChannelVm_ActiveMask(void);
uint8_t ChannelVm_MapKey(uint8_t voice, uint8_t key);
float ChannelVm_TuningScale(uint8_t voice);
FwVmFault ChannelVm_Fault(uint8_t voice);
const FwVmMetrics *ChannelVm_Metrics(uint8_t voice);
const FwVmMemoryMetrics *ChannelVm_MemoryMetrics(void);

void ChannelVm_BoundaryBegin(void);
void ChannelVm_RecordCycles(uint8_t voice, uint32_t cycles);
int ChannelVm_Dispatch(FwVmChannelHandler handler, uint8_t voice);

int ChannelVm_UploadBegin(uint8_t voice);
int ChannelVm_UploadFeed(uint8_t voice, const void *data, size_t size);
int ChannelVm_UploadCommit(uint8_t voice);
void ChannelVm_UploadAbort(uint8_t voice);
uint8_t ChannelVm_UploadIsActive(uint8_t voice);
uint8_t ChannelVm_UploadIsBusy(void);

#endif
