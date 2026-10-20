/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

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
void init_voice_envelopes(void);
void stop_voice_envelope(uint8_t voice);
/* Finite amplitudes and ramp targets are clamped to 0..1. */
int set_voice_amplitude(uint8_t voice, float amplitude);
/* Slope magnitude is speed; zero jumps to target and queues ramp completion. */
int start_voice_amplitude_ramp(uint8_t voice, float target, float slope);
float get_voice_amplitude(uint8_t voice);
float render_voice_envelope_sample(uint8_t voice);
uint8_t take_voice_ramp_completion(uint8_t voice);

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

void init_channel_scripts(const ChannelVmNativeOps *ops);
void stop_channel_script(uint8_t voice);
void stop_all_channel_scripts(void);
void reset_all_channel_script_state(void);
uint8_t is_channel_script_active(uint8_t voice);
uint8_t get_active_channel_script_mask(void);
FwVmFault get_channel_script_fault(uint8_t voice);
const FwVmMetrics *get_channel_script_metrics(uint8_t voice);
const FwVmMemoryMetrics *get_channel_script_memory_metrics(void);

void begin_channel_script_audio_block(void);
void record_channel_script_cycles(uint8_t voice, uint32_t cycles);
int dispatch_channel_script_handler(FwVmChannelHandler handler, uint8_t voice);

int begin_channel_script_upload(uint8_t voice);
int append_channel_script_upload_bytes(uint8_t voice, const void *data, size_t size);
int commit_channel_script_upload(uint8_t voice);
void abort_channel_script_upload(uint8_t voice);
uint8_t is_channel_script_upload_active(uint8_t voice);
uint8_t is_channel_script_upload_busy(void);

/* ---- control sample voices ---------------------------------------------- */

/* SAMPLE voice bank: one playhead over attack RAM + body slots. */

/** Voices available in SAMPLE mode (n0..n7). */
#define NOTE_BANK_VOICES SAMPLE_VOICES

/* ---- note bank lifecycle ------------------------------------------------ */

void init_voices(void);

void stop_all_voices_immediately(void);

/** Queue a hard stop, Berry state reset and LED-off for the next audio boundary.
 * Loaded programs and settings are preserved. Note-on returns busy until
 * this request has been consumed. */
void request_stop_all_voices(void);

/** Raw MIDI-key/velocity note-on. Script selects playback pitch when it starts the pending note. */
int queue_voice_note(uint8_t note, uint8_t key, uint8_t velocity);

/** Streamed note-on variant. The session is bound to nX before its ACK so
 * only the matching USB SOF can claim the replacement ring. */
int queue_streamed_voice_note(uint8_t note, uint8_t key, uint8_t velocity, uint8_t session);
/** Assign a sample and arm its streamed note in one control operation. */
int queue_streamed_sample_note(uint8_t note, uint16_t sample, uint8_t key, uint8_t velocity,
                               uint8_t session);
int queue_voice_note_off(uint8_t note);

uint8_t get_voice_key(uint8_t note);
uint8_t get_voice_velocity(uint8_t note);
double get_voice_frequency_hz(uint8_t note);

/** Assign sample attack 0..247 to voice 0..7. Applied on next note-on. */
int set_voice_sample_id(uint8_t note, uint16_t wave_id);
uint16_t get_voice_sample_id(uint8_t note);

/** Body FIFO miss counter. Production firmware halts on the first miss. */
uint32_t get_voice_underrun_count(void);
void clear_voice_underrun_count(void);

/** True while attack/sustain/release still sounds (incl. env release). */
uint8_t is_voice_active(uint8_t note);
uint8_t has_active_voices(void);
/** True when active, pending, or queued note state may reference the bank. */
uint8_t has_voice_sample_references(void);
int32_t mix_next_audio_sample(void);

/** Internal VM hooks bracketing one application processing interval. */
void begin_voice_audio_block(void);
void end_voice_audio_block(void);
void stop_voice_script(uint8_t voice);
void stop_all_voice_scripts(void);
uint8_t is_voice_script_active(uint8_t voice);
uint8_t get_active_voice_script_mask(void);
/** Chunked FWSC upload. Invalid uploads preserve the active program. */
int begin_voice_script_upload(uint8_t voice);
int append_voice_script_upload_bytes(uint8_t voice, const void *data, size_t size);
int commit_voice_script_upload(uint8_t voice);
void abort_voice_script_upload(uint8_t voice);
uint8_t is_voice_script_upload_active(uint8_t voice);
uint8_t is_voice_script_upload_busy(void);
const FwVmMemoryMetrics *get_voice_script_memory_metrics(void);
FwVmFault get_voice_script_fault(uint8_t voice);
uint32_t get_voice_script_max_cycles(uint8_t voice);
uint32_t get_voice_script_fault_count(uint8_t voice);

/** Playback duration until a BODY miss at the current observed rate. */
uint32_t get_voice_buffered_time_us(uint8_t voice);

/** Card-side source-sample refill budget for a 5 ms playback interval. */
uint16_t get_voice_refill_samples_5ms(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif
