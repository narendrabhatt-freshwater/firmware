#ifndef CHANNEL_APP_VOICE_H
#define CHANNEL_APP_VOICE_H

#include <stdint.h>
#include <stddef.h>
#include "vm.h"
#include "vm_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- control voice envelopes -------------------------------------------- */

#define NOTE_ENV_VOICE_COUNT 8u

/* Native sample-rate amplitude ramp used by the Channel VM adapter. Musical
 * envelope policy and section sequencing belong exclusively to VM programs. */
void NoteEnv_Init(void);
void NoteEnv_Stop(uint8_t voice);
/* Finite amplitudes and ramp targets are clamped to 0..1. */
int NoteEnv_SetAmplitude(uint8_t voice, float amplitude);
/* Slope magnitude is speed; zero jumps to target and queues ramp completion. */
int NoteEnv_StartRamp(uint8_t voice, float target, float slope);
float NoteEnv_Amplitude(uint8_t voice);
float NoteEnv_RenderSample(uint8_t voice);
uint8_t NoteEnv_TakeRampEnd(uint8_t voice);

/* ---- control voice scripts ---------------------------------------------- */

typedef struct {
    void *context;
    int (*read_input)(void *context, uint8_t note, FwVmChannelInput input, float *value);
    int (*set_amplitude)(void *context, uint8_t note, float amplitude);
    int (*ramp)(void *context, uint8_t note, float target, float slope);
    int (*note_end)(void *context, uint8_t note);
    void (*silence_voice)(void *context, uint8_t voice, FwVmFault fault);
    int (*set_led)(void *context, uint8_t voice, float red, float green, float blue,
                   float brightness);
    int (*discard_pending)(void *context, uint8_t note);
    int (*start_note_at)(void *context, uint8_t note, float frequency_hz);
    int (*osc)(void *context, uint8_t note, uint8_t wave, float frequency_hz, uint32_t *handle_out);
    int (*route)(void *context, uint8_t note, uint32_t source_handle, int32_t target,
                 uint8_t parameter, float gain);
} ChannelVmNativeOps;

void ChannelVm_Init(const ChannelVmNativeOps *ops);
void ChannelVm_Stop(uint8_t voice);
void ChannelVm_StopAll(void);
void ChannelVm_ResetStateAll(void);
uint8_t ChannelVm_IsActive(uint8_t voice);
uint8_t ChannelVm_ActiveMask(void);
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

/* ---- control sample voices ---------------------------------------------- */

/* SAMPLE voice bank: one playhead over attack RAM + body slots. */

/** Voices available in SAMPLE mode (n0..n7). */
#define NOTE_BANK_VOICES SAMPLE_VOICES

/* ---- note bank lifecycle ------------------------------------------------ */

void NoteBank_Init(void);

void NoteBank_PanicAll(void);

/** Queue a hard stop, Berry state reset and LED-off for the next audio boundary.
 * Loaded programs and settings are preserved. Note-on returns busy until
 * this request has been consumed. */
void NoteBank_AllNotesOff(void);

/** Raw MIDI-key/velocity note-on. Script selects playback pitch when it starts the pending note. */
int NoteBank_NoteOn(uint8_t note, uint8_t key, uint8_t velocity);

/** Streamed note-on variant. The session is bound to nX before its ACK so
 * only the matching USB SOF can claim the replacement ring. */
int NoteBank_NoteOnSession(uint8_t note, uint8_t key, uint8_t velocity, uint8_t session);
/** Assign a sample and arm its streamed note in one control operation. */
int NoteBank_NoteOnSampleSession(uint8_t note, uint16_t sample, uint8_t key, uint8_t velocity,
                                 uint8_t session);
int NoteBank_NoteOff(uint8_t note);

uint8_t NoteBank_GetKey(uint8_t note);
uint8_t NoteBank_GetVelocity(uint8_t note);
double NoteBank_GetFreq(uint8_t note);

/** Assign sample attack 0..247 to voice 0..7. Applied on next note-on. */
int NoteBank_SetWaveId(uint8_t note, uint16_t wave_id);
uint16_t NoteBank_GetWaveId(uint8_t note);

/** Body FIFO miss counter. Production firmware halts on the first miss. */
uint32_t NoteBank_HoldCount(void);
void NoteBank_HoldCountClear(void);

/** True while attack/sustain/release still sounds (incl. env release). */
uint8_t NoteBank_IsActive(uint8_t note);
uint8_t NoteBank_AnyActive(void);
/** True when active, pending, or queued note state may reference the bank. */
uint8_t NoteBank_AnyBankReferences(void);
int32_t NoteBank_NextSample(void);

/** VM hooks bracketing one 48-sample DMA refill. */
void NoteBank_VmBoundaryBegin(void);
void NoteBank_VmBoundaryEnd(void);
void NoteBank_VmStop(uint8_t voice);
void NoteBank_VmStopAll(void);
uint8_t NoteBank_VmIsActive(uint8_t voice);
uint8_t NoteBank_VmActiveMask(void);
/** Chunked FWSC upload. Invalid uploads preserve the active program. */
int NoteBank_VmUploadBegin(uint8_t voice);
int NoteBank_VmUploadFeed(uint8_t voice, const void *data, size_t size);
int NoteBank_VmUploadCommit(uint8_t voice);
void NoteBank_VmUploadAbort(uint8_t voice);
uint8_t NoteBank_VmUploadIsActive(uint8_t voice);
uint8_t NoteBank_VmUploadIsBusy(void);
const FwVmMemoryMetrics *NoteBank_VmMemoryMetrics(void);
FwVmFault NoteBank_VmFault(uint8_t voice);
uint32_t NoteBank_VmMaxCycles(uint8_t voice);
uint32_t NoteBank_VmFaultCount(uint8_t voice);

/** Playback duration until a BODY miss at the current observed rate. */
uint32_t NoteBank_RemainingUs(uint8_t voice);

/** Card-side source-sample refill budget for a 5 ms playback interval. */
uint16_t NoteBank_RefillSamples5ms(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif
