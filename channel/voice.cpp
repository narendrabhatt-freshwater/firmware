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

#include "voice.h"
#include "platform.h"
#include <hardware.h>
#include "application.h"
#include "filter.h"
#include "oscillator.h"
#include "samples.h"
#include "stream.h"
#include "usb.h"
#include "audio.h"
#include <math.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- render voice envelopes --------------------------------------------- */

typedef struct {
    float amplitude;
    float target;
    float step;
    uint8_t ramp_active;
    uint8_t ramp_end_pending;
} NoteEnvVoice;

static NoteEnvVoice s_voice[NOTE_ENV_VOICE_COUNT];

/* ---- initialize voice envelopes ----------------------------------------- */

void init_voice_envelopes(void)
{
    memset(s_voice, 0, sizeof(s_voice));
}

void stop_voice_envelope(uint8_t voice)
{
    if (voice >= NOTE_ENV_VOICE_COUNT) return;
    memset(&s_voice[voice], 0, sizeof(s_voice[voice]));
}

/* ---- set amplitude and schedule ramps ----------------------------------- */

int set_voice_amplitude(uint8_t voice, float amplitude)
{
    NoteEnvVoice *envelope;
    if (voice >= NOTE_ENV_VOICE_COUNT || !isfinite(amplitude)) return -1;
    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    envelope = &s_voice[voice];
    envelope->amplitude = amplitude;
    envelope->target = amplitude;
    envelope->step = 0.0f;
    envelope->ramp_active = 0u;
    envelope->ramp_end_pending = 0u;
    return 0;
}

int start_voice_amplitude_ramp(uint8_t voice, float target, float slope)
{
    NoteEnvVoice *envelope;
    float step;
    if (voice >= NOTE_ENV_VOICE_COUNT || !isfinite(target) || !isfinite(slope)) return -1;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    if (slope == 0.0f) {
        (void)set_voice_amplitude(voice, target);
        s_voice[voice].ramp_end_pending = 1u;
        return 0;
    }
    step = fabsf(slope) / (float)AUDIO_SAMPLE_RATE_HZ;
    if (step > 1.0f) step = 1.0f;
    if (step == 0.0f) step = FLT_MIN;
    envelope = &s_voice[voice];
    envelope->target = target;
    envelope->step =
        envelope->amplitude < target ? step : (envelope->amplitude > target ? -step : 0.0f);
    envelope->ramp_active = envelope->amplitude != target;
    envelope->ramp_end_pending = envelope->amplitude == target;
    return 0;
}

float get_voice_amplitude(uint8_t voice)
{
    return voice < NOTE_ENV_VOICE_COUNT ? s_voice[voice].amplitude : 0.0f;
}

/* ---- advance the amplitude envelope ------------------------------------- */

float render_voice_envelope_sample(uint8_t voice)
{
    NoteEnvVoice *envelope;
    float next;
    float tolerance;
    if (voice >= NOTE_ENV_VOICE_COUNT) return 0.0f;
    envelope = &s_voice[voice];
    if (envelope->ramp_active == 0u) return envelope->amplitude;
    next = envelope->amplitude + envelope->step;
    tolerance = fabsf(envelope->step) * 0.001f + 0.0000001f;
    if (envelope->step == 0.0f || (envelope->step > 0.0f && next + tolerance >= envelope->target) ||
        (envelope->step < 0.0f && next - tolerance <= envelope->target)) {
        envelope->amplitude = envelope->target;
        envelope->step = 0.0f;
        envelope->ramp_active = 0u;
        envelope->ramp_end_pending = 1u;
    } else {
        envelope->amplitude = next;
    }
    return envelope->amplitude;
}

/* ---- consume ramp completion -------------------------------------------- */

uint8_t take_voice_ramp_completion(uint8_t voice)
{
    uint8_t pending;
    if (voice >= NOTE_ENV_VOICE_COUNT) return 0u;
    pending = s_voice[voice].ramp_end_pending;
    s_voice[voice].ramp_end_pending = 0u;
    return pending;
}

/* ---- dispatch voice script handlers ------------------------------------- */

extern "C" {
#include "berry_backend.h"
}

static ScriptBerryRuntime s_runtime CHANNEL_VM_STORAGE;

/* ---- initialize the shared script runtime ------------------------------- */

void init_channel_scripts(const ChannelVmNativeOps *ops)
{
    ScriptBerryNativeOps native_ops;
    memset(&native_ops, 0, sizeof(native_ops));
    if (ops != NULL) {
        native_ops.context = ops->context;
        native_ops.read_input = ops->read_input;
        native_ops.set_amplitude = ops->set_amplitude;
        native_ops.ramp = ops->ramp;
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

/* ---- stop and reset voice programs -------------------------------------- */

void stop_channel_script(uint8_t voice)
{
    script_berry_stop(&s_runtime, voice);
}

void stop_all_channel_scripts(void)
{
    script_berry_stop_all(&s_runtime);
}

void reset_all_channel_script_state(void)
{
    script_berry_reset_state_all(&s_runtime);
}

/* ---- query script state and diagnostics --------------------------------- */

uint8_t is_channel_script_active(uint8_t voice)
{
    return script_berry_is_active(&s_runtime, voice);
}

uint8_t get_active_channel_script_mask(void)
{
    return script_berry_active_mask(&s_runtime);
}

FwVmFault get_channel_script_fault(uint8_t voice)
{
    return script_berry_fault(&s_runtime, voice);
}

const FwVmMetrics *get_channel_script_metrics(uint8_t voice)
{
    return script_berry_voice_metrics(&s_runtime, voice);
}

const FwVmMemoryMetrics *get_channel_script_memory_metrics(void)
{
    return script_berry_memory_metrics(&s_runtime);
}

/* ---- account for audio boundary work ------------------------------------ */

void begin_channel_script_audio_block(void)
{
    script_berry_boundary_begin(&s_runtime);
}

void record_channel_script_cycles(uint8_t voice, uint32_t cycles)
{
    script_berry_record_cycles(&s_runtime, voice, cycles);
}

/* ---- dispatch a voice handler ------------------------------------------- */

int dispatch_channel_script_handler(FwVmChannelHandler handler, uint8_t voice)
{
    return script_berry_dispatch(&s_runtime, handler, voice);
}

/* ---- upload a voice program --------------------------------------------- */

int begin_channel_script_upload(uint8_t voice)
{
    return script_berry_upload_begin(&s_runtime, voice);
}

int append_channel_script_upload_bytes(uint8_t voice, const void *data, size_t size)
{
    return script_berry_upload_is_active(&s_runtime, voice)
               ? script_berry_upload_feed(&s_runtime, data, size)
               : -1;
}

int commit_channel_script_upload(uint8_t voice)
{
    return script_berry_upload_is_active(&s_runtime, voice) ? script_berry_upload_commit(&s_runtime)
                                                            : -1;
}

void abort_channel_script_upload(uint8_t voice)
{
    if (script_berry_upload_is_active(&s_runtime, voice)) script_berry_upload_abort(&s_runtime);
}

uint8_t is_channel_script_upload_active(uint8_t voice)
{
    return script_berry_upload_is_active(&s_runtime, voice);
}

uint8_t is_channel_script_upload_busy(void)
{
    return s_runtime.upload_active;
}

/* ---- render sample voices ----------------------------------------------- */

/* SAMPLE voices: attack RAM + body slots → sample → LPF → env.
 * Attack is an AXI head of committed length (not hold-padded). Body is
 * the USB BODY FIFO. Pitch uses 2-tap linear interpolation (Q16.16). At the
 * join, the body playhead is locked to the attack source index so both sides
 * read the same sample. nX > 0 is always a note-on. Playhead changes apply on
 * the next rendered sample. */

static_assert(NOTE_FILTER_VOICES >= NOTE_BANK_VOICES,
              "note_filter voice count must cover note_bank");
static_assert(NOTE_ENV_VOICE_COUNT == NOTE_BANK_VOICES,
              "note_envelope voice count must match note_bank");
static_assert(STREAM_RING_SAMPLES == 4080u,
              "normal streaming profile must keep 85 ms per BODY ring");

#define NOTE_AMP_Q15_MAX 32767
#define PHASE_ONE (1u << 16)
#define PHASE_INC_MIN (PHASE_ONE / 16u)
#define PHASE_INC_MAX (PHASE_ONE * 2u)
/* ~1 octave in 20 ms @ 48 kHz. */
#define PHASE_INC_SLEW 68u
#define INTERP_LEFT_TAPS 0u
#define BODY_ADVANCE_PHASE ((INTERP_LEFT_TAPS + 1u) * PHASE_ONE)
#define NOTE_SAMPLE_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#define NOTE_DEFAULT_SCALE 0.125
#define BODY_LOOP_SAMPLES 256u

static double note_freq_hz[NOTE_BANK_VOICES];
static double note_scale[NOTE_BANK_VOICES];
static int32_t note_amp_q15[NOTE_BANK_VOICES];
static int32_t note_play_amp_q15[NOTE_BANK_VOICES];
static uint8_t note_active[NOTE_BANK_VOICES];
/* Q16.16 attack index. Body phase is relative to the ring read pointer. */
static uint64_t note_phase[NOTE_BANK_VOICES];
static uint32_t note_body_frac[NOTE_BANK_VOICES];
static int32_t note_hold[NOTE_BANK_VOICES];
/* Preserve recent source samples after the stream ring frees their storage. */
static int8_t note_body_loop[NOTE_BANK_VOICES][BODY_LOOP_SAMPLES];
static uint32_t note_loop_wr[NOTE_BANK_VOICES];
static uint32_t note_loop_count[NOTE_BANK_VOICES];
static uint32_t note_loop_phase[NOTE_BANK_VOICES];
static uint32_t note_inc[NOTE_BANK_VOICES];
static uint32_t note_observed_inc[NOTE_BANK_VOICES];
static uint32_t note_inc_tgt[NOTE_BANK_VOICES];
/* Body consume starts at fade0. skip = source samples already past. */
static uint8_t note_body_locked[NOTE_BANK_VOICES];
static uint32_t note_body_skip[NOTE_BANK_VOICES];
static uint16_t note_wave_id[NOTE_BANK_VOICES];
static uint16_t note_play_wid[NOTE_BANK_VOICES];
static uint16_t note_play_alen[NOTE_BANK_VOICES];
static uint8_t note_body_only[NOTE_BANK_VOICES];
static uint32_t note_next_inc[NOTE_BANK_VOICES];
static float note_next_hz[NOTE_BANK_VOICES];
static int32_t note_next_amp_q15[NOTE_BANK_VOICES];
static uint16_t note_next_wid[NOTE_BANK_VOICES];
static uint8_t note_next_pending[NOTE_BANK_VOICES];
static float note_play_hz[NOTE_BANK_VOICES];
static uint8_t note_play_key[NOTE_BANK_VOICES];
static uint8_t note_play_velocity[NOTE_BANK_VOICES];
static uint8_t note_next_key[NOTE_BANK_VOICES];
static uint8_t note_next_velocity[NOTE_BANK_VOICES];
static volatile uint32_t note_hold_miss;

#define NOTE_CMD_NONE 0u
#define NOTE_CMD_ON 1u
#define NOTE_CMD_OFF 2u
#define NOTE_CMD_REL 3u

/* Console/USB posts; the next audio block applies. Last command wins. */
static volatile uint8_t note_cmd[NOTE_BANK_VOICES];
static volatile uint8_t note_cmd_key[NOTE_BANK_VOICES];
static volatile uint8_t note_cmd_velocity[NOTE_BANK_VOICES];
/* An explicit nX off must win over a late BODY session-start. */
static volatile uint8_t note_gate_requested[NOTE_BANK_VOICES];
static volatile uint8_t note_all_off_requested;

static inline int32_t clamp_voice_mix(int64_t sum);
static int dispatch_voice_script_handler(FwVmChannelHandler handler, uint8_t note);

/* ---- convert note gain and playback rate -------------------------------- */

static int32_t voice_amplitude_to_q15(double scale)
{
    if (scale <= 0.0) {
        return 0;
    }
    if (scale >= 1.0) {
        return NOTE_AMP_Q15_MAX;
    }
    return (int32_t)(scale * (double)NOTE_AMP_Q15_MAX + 0.5);
}

/* Preserve pitch class while limiting streamed sample consumption to 2x.
 * Fold before quantization so notes above the former 16x limit stay in tune. */
static uint32_t fold_voice_phase_increment(double inc)
{
    if (!isfinite(inc)) return inc > 0.0 ? PHASE_INC_MAX : PHASE_INC_MIN;
    while (inc > (double)PHASE_INC_MAX)
        inc *= 0.5;
    if (inc < (double)PHASE_INC_MIN) inc = PHASE_INC_MIN;
    return (uint32_t)(inc + 0.5);
}

static uint32_t voice_frequency_to_phase_increment(uint16_t wave_id, double freq_hz)
{
    float root;
    double inc;

    root = get_sample_root_frequency_hz(wave_id);
    if (!(root > 0.0f) || !(freq_hz > 0.0)) {
        return PHASE_ONE;
    }
    inc = freq_hz / (double)root;
    return fold_voice_phase_increment(inc * (double)PHASE_ONE);
}

/* ---- interpolate the attack and body boundary --------------------------- */

static int32_t interpolate_voice_attack(uint16_t wid, uint64_t phase)
{
    const int8_t *tab = get_sample_data(wid);
    uint32_t len = get_sample_length(wid);
    uint32_t phase_q16;
    uint32_t i0;
    uint32_t i1;
    uint32_t frac;
    int32_t s0;
    int32_t s1;

    if (tab == NULL || len == 0u) {
        return 0;
    }
    /* Attack heads are ≤ ATTACK_BANK_LEN; Q16.16 index fits in 32 bits. */
    phase_q16 = (phase > 0xffffffffull) ? 0xffffffffu : (uint32_t)phase;
    i0 = phase_q16 >> 16;
    if (i0 >= len) {
        i0 = len - 1u;
    }
    i1 = (i0 + 1u < len) ? i0 + 1u : i0;
    frac = phase_q16 & 0xFFFFu;
    s0 = (int32_t)tab[i0] * 16777216;
    s1 = (int32_t)tab[i1] * 16777216;
    return (int32_t)((int64_t)s0 + (((int64_t)s1 - (int64_t)s0) * frac >> 16));
}

static int interpolate_voice_attack_body_transition(uint8_t note, uint16_t wid, uint64_t phase,
                                                    int32_t *out)
{
    const int8_t *attack = get_sample_data(wid);
    uint32_t alen = get_sample_length(wid);
    int32_t source_i0 = (int32_t)(phase >> 16);
    int32_t body_i0 = (int32_t)(note_body_frac[note] >> 16);
    int32_t taps[2];
    uint32_t t;

    if (out == NULL || attack == NULL || alen == 0u) {
        return -1;
    }
    for (t = 0u; t < 2u; t++) {
        int32_t source = source_i0 + (int32_t)t;
        if (source < 0) {
            source = 0;
        }
        if ((uint32_t)source < alen) {
            taps[t] = (int32_t)attack[source] * 16777216;
        } else {
            int32_t offset = body_i0 + source - source_i0;
            int8_t sample;
            if (offset < 0 || peek_stream_sample(note, (uint32_t)offset, &sample) != 0) {
                return -1;
            }
            taps[t] = (int32_t)sample * 16777216;
        }
    }
    *out = (int32_t)((int64_t)taps[0] +
                     (((int64_t)taps[1] - (int64_t)taps[0]) * ((uint32_t)phase & 0xFFFFu) >> 16));
    return 0;
}

/* ---- retain body interpolation history ---------------------------------- */

static void remember_voice_body_sample(uint8_t note, int8_t sample)
{
    note_body_loop[note][note_loop_wr[note]] = sample;
    note_loop_wr[note] = (note_loop_wr[note] + 1u) % BODY_LOOP_SAMPLES;
    if (note_loop_count[note] < BODY_LOOP_SAMPLES) note_loop_count[note]++;
    note_loop_phase[note] = 0u;
}

/**
 * A required BODY sample is unavailable in the stream ring. Count the
 * underrun and repeat the recent buffer until new samples are available.
 */
static int32_t render_voice_body_underrun(uint8_t note, uint32_t increment)
{
    int8_t sample;
    uint32_t count;
    uint32_t start;
    uint32_t index;
    uint32_t next;
    uint32_t phase;
    int32_t s0;
    int32_t s1;
    note_hold_miss++;
    /* Include any remaining tail sample before releasing its ring storage. */
    while (peek_stream_sample(note, 0u, &sample) == 0) {
        remember_voice_body_sample(note, sample);
        skip_stream_samples(note, 1u);
    }
    /* Resume at the next received sample, without accumulating missing time. */
    note_body_frac[note] = 0u;
    note_body_skip[note] = 0u;
    note_body_only[note] = 1u;
    count = note_loop_count[note];
    if (count == 0u) return 0;
    phase = note_loop_phase[note];
    start = (note_loop_wr[note] + BODY_LOOP_SAMPLES - count) % BODY_LOOP_SAMPLES;
    index = phase >> 16;
    next = (index + 1u) % count;
    s0 = note_body_loop[note][(start + index) % BODY_LOOP_SAMPLES];
    s1 = note_body_loop[note][(start + next) % BODY_LOOP_SAMPLES];
    note_loop_phase[note] = (phase + increment) % (count * PHASE_ONE);
    return (int32_t)((int64_t)s0 * 16777216 + (int64_t)(s1 - s0) * (phase & 0xFFFFu) * 256);
}

/* ---- interpolate streamed body samples ---------------------------------- */

static int interpolate_voice_body(uint8_t note, int32_t *out)
{
    uint32_t phase = note_body_frac[note];
    uint32_t i0 = phase >> 16;
    int8_t taps[2];
    if (out == NULL || peek_stream_samples(note, i0, taps, 2u) != 0) {
        record_stream_buffered_samples(note);
        return -1;
    }
    int8_t s0 = taps[0], s1 = taps[1];
    *out = (int32_t)((int64_t)s0 * 16777216 + (int64_t)(s1 - s0) * (phase & 0xFFFFu) * 256);
    return 0;
}

/* ---- advance the sample playhead ---------------------------------------- */

static void advance_voice_body_position(uint8_t note, uint32_t increment)
{
    int8_t consumed[2];
    note_body_frac[note] += increment;
    while (note_body_frac[note] >= BODY_ADVANCE_PHASE) {
        uint32_t count = note_body_frac[note] >> 16;
        if (count > 2u) count = 2u;
        count = read_stream_samples(note, consumed, count);
        if (!count) break;
        for (uint32_t i = 0; i < count; i++)
            remember_voice_body_sample(note, consumed[i]);
        note_body_frac[note] -= count * PHASE_ONE;
    }
}

/* ---- reset and replace the voice playhead ------------------------------- */

static void clear_voice_playhead(uint8_t note)
{
    note_phase[note] = 0u;
    note_body_frac[note] = 0u;
    note_hold[note] = 0;
    note_loop_wr[note] = 0u;
    note_loop_count[note] = 0u;
    note_loop_phase[note] = 0u;
    note_body_locked[note] = 0u;
    note_body_skip[note] = 0u;
    note_body_only[note] = 0u;
}

static void activate_pending_voice(uint8_t note)
{
    if (note_next_pending[note] == 0u) {
        return;
    }
    clear_voice_playhead(note);
    note_inc[note] = note_next_inc[note];
    note_observed_inc[note] = note_next_inc[note];
    note_inc_tgt[note] = note_next_inc[note];
    note_play_amp_q15[note] = note_next_amp_q15[note];
    note_play_hz[note] = note_next_hz[note];
    note_play_key[note] = note_next_key[note];
    note_play_velocity[note] = note_next_velocity[note];
    note_play_wid[note] = note_next_wid[note];
    note_play_alen[note] = (uint16_t)get_sample_length(note_play_wid[note]);
    note_body_only[note] = (note_play_alen[note] == 0u) ? 1u : 0u;
    activate_pending_oscillators(note);
    reset_voice_filter(note);
    update_voice_filter_note_frequency(note, (double)note_next_hz[note]);
    note_next_pending[note] = 0u;
    note_next_key[note] = 0u;
    note_next_velocity[note] = 0u;
    note_active[note] = 1u;
}

static void sync_voice_body_playhead(uint8_t note, uint64_t phase, uint64_t fade0)
{
    uint64_t rel;
    uint32_t source_index;
    uint32_t history;

    if (note_body_locked[note] != 0u) {
        return;
    }
    rel = (phase >= fade0) ? (phase - fade0) : 0u;
    source_index = (uint32_t)(rel >> 16);
    history = (source_index < INTERP_LEFT_TAPS) ? source_index : INTERP_LEFT_TAPS;
    note_body_skip[note] = source_index - history;
    note_body_frac[note] = (history << 16) | (uint32_t)(rel & 0xFFFFu);
    note_body_locked[note] = 1u;
}

static void catch_up_voice_body_playhead(uint8_t note)
{
    uint32_t skip = note_body_skip[note];
    uint32_t filled;

    if (skip == 0u) {
        return;
    }
    filled = get_stream_buffered_samples(note);
    if (filled == 0u) {
        return;
    }
    if (skip > filled) {
        skip_stream_samples(note, filled);
        note_body_skip[note] = skip - filled;
        return;
    }
    skip_stream_samples(note, skip);
    note_body_skip[note] = 0u;
}

/* ---- activate a prepared voice ------------------------------------------ */

static void start_voice_playback(uint8_t note, uint8_t key, uint8_t velocity)
{
    /* Pitch is selected only when the script calls start_note([frequency]). */
    note_next_inc[note] = 0u;
    note_next_hz[note] = 0.0f;
    note_next_key[note] = key;
    note_next_velocity[note] = velocity;
    note_next_amp_q15[note] = note_amp_q15[note];
    note_next_wid[note] = note_wave_id[note];
    note_next_pending[note] = 1u;
    begin_pending_oscillators(note);
}

/* ---- queue voice events ------------------------------------------------- */

static void post_voice_note_on(uint8_t note, uint8_t key, uint8_t velocity)
{
    note_cmd_key[note] = key;
    note_cmd_velocity[note] = velocity;
    note_cmd[note] = NOTE_CMD_ON;
}

static void stop_voice_immediately(uint8_t note)
{
    note_freq_hz[note] = 0.0;
    note_active[note] = 0u;
    note_next_pending[note] = 0u;
    note_play_key[note] = 0u;
    note_play_velocity[note] = 0u;
    note_next_key[note] = 0u;
    note_next_velocity[note] = 0u;
    clear_voice_playhead(note);
    release_voice_stream(note);
    stop_sample_playback(note);
    stop_voice_oscillators(note);
    reset_voice_filter(note);
    stop_voice_envelope(note);
}

static void end_current_voice_note(uint8_t note)
{
    note_freq_hz[note] = 0.0;
    note_active[note] = 0u;
    note_play_key[note] = 0u;
    note_play_velocity[note] = 0u;
    clear_voice_playhead(note);
    end_current_voice_stream(note);
    stop_sample_playback(note);
    stop_active_oscillators(note);
    reset_voice_filter(note);
    stop_voice_envelope(note);
}

/* ---- consume pending voice commands ------------------------------------- */

static void process_pending_voice_command(uint8_t note)
{
    const uint8_t cmd = note_cmd[note];
    if (cmd == NOTE_CMD_NONE) {
        return;
    }
    if (cmd == NOTE_CMD_ON) {
        if (is_channel_script_active(note) == 0u) {
            note_cmd[note] = NOTE_CMD_NONE;
            stop_voice_immediately(note);
            return;
        }
        if (note_active[note] == 0u &&
            get_pending_stream_buffered_samples(note) < USB_STREAM_PRIME_SAMPLES)
            return;
        note_cmd[note] = NOTE_CMD_NONE;
        start_voice_playback(note, note_cmd_key[note], note_cmd_velocity[note]);
        (void)dispatch_voice_script_handler(FW_VM_CHANNEL_HANDLER_NOTE_ON, note);
        return;
    }
    note_cmd[note] = NOTE_CMD_NONE;
    if (cmd == NOTE_CMD_REL) {
        /* Note-off always cancels a not-yet-started replacement. Pending BODY is
         * transport state, so envelope scripts do not need to manage it. */
        discard_pending_voice_stream(note);
        discard_pending_oscillators(note);
        note_next_pending[note] = 0u;
        note_next_key[note] = 0u;
        note_next_velocity[note] = 0u;
        /* A late key release after a hard stop must not re-enter Berry and
         * dirty its freshly reset state. Pending startup was cancelled above. */
        if (note_active[note] == 0u || is_channel_script_active(note) == 0u)
            stop_voice_immediately(note);
        else
            (void)dispatch_voice_script_handler(FW_VM_CHANNEL_HANDLER_NOTE_OFF, note);
        return;
    }
    if (cmd == NOTE_CMD_OFF) {
        stop_voice_immediately(note);
    }
}

/* ---- render the voice sample -------------------------------------------- */

static void slew_voice_phase_increment(uint8_t note)
{
    uint32_t inc = note_inc[note];
    uint32_t tgt = note_inc_tgt[note];
    if (inc < tgt) {
        uint32_t n = inc + PHASE_INC_SLEW;
        note_inc[note] = (n > tgt) ? tgt : n;
    } else if (inc > tgt) {
        uint32_t n = (inc > PHASE_INC_SLEW) ? (inc - PHASE_INC_SLEW) : 0u;
        note_inc[note] = (n < tgt) ? tgt : n;
    }
}

/**
 * Dry sample. Head runs to its committed length. Body starts at the
 * same source index (len - overlap); the FIFO is caught up if the
 * playhead lands past that point.
 */
static int32_t render_voice_sample_at_increment(uint8_t note, uint32_t increment)
{
    uint16_t wid = note_play_wid[note];
    uint64_t phase = note_phase[note];
    int32_t y;
    uint32_t alen = note_play_alen[note];
    uint64_t fade0;
    uint64_t fade1;

    if (note_body_only[note] != 0u) {
        if (interpolate_voice_body(note, &y) != 0) {
            return render_voice_body_underrun(note, increment);
        }
        advance_voice_body_position(note, increment);
        note_phase[note] = phase + (uint64_t)increment;
        note_hold[note] = y;
        return y;
    }

    fade1 = (uint64_t)alen << 16;
    fade0 = (alen > SAMPLE_CROSSFADE_LEN) ? ((uint64_t)(alen - SAMPLE_CROSSFADE_LEN) << 16) : 0u;

    if (phase >= fade0) {
        sync_voice_body_playhead(note, phase, fade0);
        catch_up_voice_body_playhead(note);
    }

    if (phase < fade1) {
        y = interpolate_voice_attack(wid, phase);
        if (phase >= fade0 && note_body_skip[note] == 0u) {
            int32_t body;
            if (interpolate_voice_body(note, &body) == 0) {
                int32_t joined;
                if (interpolate_voice_attack_body_transition(note, wid, phase, &joined) == 0) {
                    y = joined;
                }
                advance_voice_body_position(note, increment);
            } else {
                /* The local attack is still authoritative. BODY may arrive any time
                 * before the attack exhausts; account for a miss only at the actual
                 * BODY boundary. */
            }
        }
        note_phase[note] = phase + (uint64_t)increment;
        note_hold[note] = y;
        return y;
    }

    if (note_body_skip[note] != 0u) {
        return render_voice_body_underrun(note, increment);
    }
    if (interpolate_voice_body(note, &y) != 0) {
        return render_voice_body_underrun(note, increment);
    }
    note_body_only[note] = 1u;
    advance_voice_body_position(note, increment);
    note_phase[note] = phase + (uint64_t)increment;
    note_hold[note] = y;
    return y;
}

static inline int32_t render_voice_sample(uint8_t note)
{
    int32_t s;
    float env;
    float sample_frequency_mod;
    float sample_amplitude;
    int32_t env_q31;
    int32_t gain_q31;
    int32_t amp;
    uint32_t frame_inc;
    double delta_inc;
    float root_hz;

    slew_voice_phase_increment(note);
    apply_oscillator_sample_modulation(note, &sample_frequency_mod, &sample_amplitude);
    (void)sample_amplitude;
    root_hz = get_sample_root_frequency_hz(note_play_wid[note]);
    delta_inc =
        (root_hz > 0.0f) ? (double)sample_frequency_mod * (double)PHASE_ONE / (double)root_hz : 0.0;
    delta_inc += (double)note_inc[note];
    frame_inc = fold_voice_phase_increment(delta_inc);
    note_observed_inc[note] = frame_inc;
    s = render_voice_sample_at_increment(note, frame_inc);
    s = mix_voice_oscillators(note, s);
    s = filter_voice_sample(note, s);

    env = render_voice_envelope_sample(note);
    if (env >= 1.0f) {
        env_q31 = INT32_MAX;
    } else if (env <= 0.0f) {
        env_q31 = 0;
    } else {
        env_q31 = (int32_t)(env * 2147483648.0f);
    }
    gain_q31 = (int32_t)(((int64_t)note_play_amp_q15[note] * (int64_t)env_q31) >> 15);
    amp = (int32_t)(((int64_t)s * (int64_t)gain_q31) >> 31);
    return amp;
}

static inline int32_t clamp_voice_mix(int64_t sum)
{
    if (sum > (int64_t)0x7FFFFFFF) {
        return (int32_t)0x7FFFFFFF;
    }
    if (sum < (int64_t)(int32_t)0x80000000) {
        return (int32_t)(int32_t)0x80000000;
    }
    return (int32_t)sum;
}

/* ---- expose voice inputs to berry --------------------------------------- */

static int read_voice_script_input(void *context, uint8_t note, FwVmChannelInput value, float *out)
{
    (void)context;
    if (out == NULL || note >= NOTE_BANK_VOICES) return -1;
    switch (value) {
    case FW_VM_CHANNEL_INPUT_NOTE_ID:
        *out = (float)note;
        break;
    case FW_VM_CHANNEL_INPUT_FREQUENCY:
        *out = (note_active[note] != 0u) ? note_play_hz[note] : note_next_hz[note];
        break;
    case FW_VM_CHANNEL_INPUT_GAIN:
        *out =
            (float)((note_active[note] != 0u) ? note_play_amp_q15[note] : note_next_amp_q15[note]) /
            (float)NOTE_AMP_Q15_MAX;
        break;
    case FW_VM_CHANNEL_INPUT_GATE:
        *out = note_gate_requested[note] != 0u ? 1.0f : 0.0f;
        break;
    case FW_VM_CHANNEL_INPUT_ACTIVE:
        *out = note_active[note] != 0u ? 1.0f : 0.0f;
        break;
    case FW_VM_CHANNEL_INPUT_HAS_PENDING:
        *out = note_next_pending[note] != 0u ? 1.0f : 0.0f;
        break;
    case FW_VM_CHANNEL_INPUT_PENDING_FREQUENCY:
        *out = note_next_hz[note];
        break;
    case FW_VM_CHANNEL_INPUT_PENDING_GAIN:
        *out = (float)note_next_amp_q15[note] / (float)NOTE_AMP_Q15_MAX;
        break;
    case FW_VM_CHANNEL_INPUT_AMPLITUDE:
        *out = get_voice_amplitude(note);
        break;
    case FW_VM_CHANNEL_INPUT_KEY:
        *out = (float)note_play_key[note];
        break;
    case FW_VM_CHANNEL_INPUT_PENDING_KEY:
        *out = (float)note_next_key[note];
        break;
    case FW_VM_CHANNEL_INPUT_VELOCITY:
        *out = (float)note_play_velocity[note];
        break;
    case FW_VM_CHANNEL_INPUT_PENDING_VELOCITY:
        *out = (float)note_next_velocity[note];
        break;
    default:
        return -1;
    }
    return 0;
}

/* ---- apply script envelope and note operations -------------------------- */

static int set_voice_script_amplitude(void *context, uint8_t note, float amplitude)
{
    (void)context;
    return set_voice_amplitude(note, amplitude);
}

static int start_voice_script_amplitude_ramp(void *context, uint8_t note, float target, float slope)
{
    (void)context;
    return start_voice_amplitude_ramp(note, target, slope);
}

static int start_voice_script_note_at_frequency(void *context, uint8_t note, float frequency_hz)
{
    uint32_t inc;
    (void)context;
    if (note >= NOTE_BANK_VOICES || note_next_pending[note] == 0u || !isfinite(frequency_hz) ||
        !(frequency_hz > 0.0f)) {
        return -1;
    }
    inc = voice_frequency_to_phase_increment(note_next_wid[note], (double)frequency_hz);
    if (inc < PHASE_INC_MIN) inc = PHASE_INC_MIN;
    if (inc > PHASE_INC_MAX) inc = PHASE_INC_MAX;
    if (finalize_pending_oscillators(note) != 0) return -1;
    if (activate_pending_voice_stream(note) != 0) return -1;
    note_next_hz[note] = frequency_hz;
    note_next_inc[note] = inc;
    activate_pending_voice(note);
    note_freq_hz[note] = note_play_hz[note];
    return 0;
}

static int end_voice_script_note(void *context, uint8_t note)
{
    (void)context;
    end_current_voice_note(note);
    return 0;
}

static int discard_voice_script_pending_note(void *context, uint8_t note)
{
    (void)context;
    if (note >= NOTE_BANK_VOICES) return -1;
    discard_pending_voice_stream(note);
    discard_pending_oscillators(note);
    note_next_pending[note] = 0u;
    note_next_key[note] = 0u;
    note_next_velocity[note] = 0u;
    return 0;
}

static void silence_voice_after_script_fault(void *context, uint8_t note, FwVmFault fault)
{
    (void)context;
    (void)fault;
    stop_voice_immediately(note);
}

static int set_voice_script_led(void *context, uint8_t note, float red, float green, float blue,
                                float brightness)
{
    (void)context;
    (void)note;
    return set_channel_led_colour(red, green, blue, brightness);
}

/* ---- declare script oscillators and routes ------------------------------ */

static int add_voice_script_oscillator(void *context, uint8_t note, uint8_t wave,
                                       float frequency_hz, uint32_t *handle_out)
{
    (void)context;
    if (note >= NOTE_BANK_VOICES || note_next_pending[note] == 0u) {
        return -1;
    }
    return add_pending_oscillator(note, wave, frequency_hz, handle_out);
}

static int add_voice_script_route(void *context, uint8_t note, uint32_t source_handle,
                                  int32_t target, uint8_t parameter, float gain)
{
    (void)context;
    if (note >= NOTE_BANK_VOICES || note_next_pending[note] == 0u) return -1;
    return add_pending_oscillator_route(note, source_handle, target, parameter, gain);
}

/* ---- dispatch voice events to berry ------------------------------------- */

static int dispatch_voice_script_handler(FwVmChannelHandler handler, uint8_t note)
{
#if defined(CHANNEL_ENABLE_CYCLE_PROFILING)
    uint32_t start = get_board_cycle_count();
#endif
    int result = dispatch_channel_script_handler(handler, note);
#if defined(CHANNEL_ENABLE_CYCLE_PROFILING)
    record_channel_script_cycles(note, get_board_cycle_count() - start);
#endif
    return result;
}

/* ---- initialize the note bank ------------------------------------------- */

void init_voices(void)
{
    uint8_t i;
    ChannelVmNativeOps vm_ops = {};
    note_all_off_requested = 0u;

    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        note_freq_hz[i] = 0.0;
        note_scale[i] = 0.0;
        note_amp_q15[i] = 0;
        note_play_amp_q15[i] = 0;
        note_active[i] = 0u;
        note_phase[i] = 0u;
        note_body_frac[i] = 0u;
        note_hold[i] = 0;
        note_loop_wr[i] = 0u;
        note_loop_count[i] = 0u;
        note_loop_phase[i] = 0u;
        note_inc[i] = PHASE_ONE;
        note_inc_tgt[i] = PHASE_ONE;
        note_body_locked[i] = 0u;
        note_body_skip[i] = 0u;
        note_wave_id[i] = (uint16_t)i;
        note_play_wid[i] = (uint16_t)i;
        note_play_alen[i] = 0u;
        note_body_only[i] = 0u;
        note_next_inc[i] = PHASE_ONE;
        note_next_hz[i] = 0.0f;
        note_next_amp_q15[i] = 0;
        note_next_wid[i] = (uint16_t)i;
        note_next_pending[i] = 0u;
        note_play_hz[i] = 0.0f;
        note_play_key[i] = 0u;
        note_play_velocity[i] = 0u;
        note_next_key[i] = 0u;
        note_next_velocity[i] = 0u;
        note_cmd[i] = NOTE_CMD_NONE;
        note_cmd_velocity[i] = 0u;
        note_gate_requested[i] = 0u;
    }
    vm_ops.context = NULL;
    vm_ops.read_input = read_voice_script_input;
    vm_ops.set_amplitude = set_voice_script_amplitude;
    vm_ops.ramp = start_voice_script_amplitude_ramp;
    vm_ops.note_end = end_voice_script_note;
    vm_ops.silence_voice = silence_voice_after_script_fault;
    vm_ops.set_led = set_voice_script_led;
    vm_ops.discard_pending = discard_voice_script_pending_note;
    vm_ops.start_note_at = start_voice_script_note_at_frequency;
    vm_ops.osc = add_voice_script_oscillator;
    vm_ops.route = add_voice_script_route;
#if defined(CHANNEL_ENABLE_CYCLE_PROFILING)
    init_board_cycle_counter();
#endif
    init_channel_scripts(&vm_ops);
    init_wavetable_oscillators();
}

/* ---- release and panic voices ------------------------------------------- */

void request_stop_all_voices(void)
{
    note_all_off_requested = 1u;
}

void stop_all_voices_immediately(void)
{
    uint8_t i;

    stop_all_sample_playback();
    stop_all_oscillators();
    reset_all_voice_streams();
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        note_freq_hz[i] = 0.0;
        note_scale[i] = 0.0;
        note_amp_q15[i] = 0;
        note_play_amp_q15[i] = 0;
        note_active[i] = 0u;
        note_phase[i] = 0u;
        note_body_frac[i] = 0u;
        note_hold[i] = 0;
        note_loop_wr[i] = 0u;
        note_loop_count[i] = 0u;
        note_loop_phase[i] = 0u;
        note_inc[i] = PHASE_ONE;
        note_inc_tgt[i] = PHASE_ONE;
        note_body_locked[i] = 0u;
        note_body_skip[i] = 0u;
        note_play_alen[i] = 0u;
        note_body_only[i] = 0u;
        note_next_pending[i] = 0u;
        note_cmd[i] = NOTE_CMD_NONE;
        note_gate_requested[i] = 0u;
        note_play_key[i] = 0u;
        note_play_velocity[i] = 0u;
        note_next_key[i] = 0u;
        note_next_velocity[i] = 0u;
        note_cmd_velocity[i] = 0u;
        reset_voice_filter(i);
        stop_voice_envelope(i);
    }
}

/* ---- queue note-on and note-off commands -------------------------------- */

static int queue_voice_note_with_session(uint8_t note, uint8_t key, uint8_t velocity,
                                         uint8_t session)
{
    double scale = NOTE_DEFAULT_SCALE;

    if (note >= NOTE_BANK_VOICES || key >= FW_SCRIPT_CHANNEL_KEY_COUNT || velocity == 0u ||
        velocity > 127u) {
        return -1;
    }
    if (is_channel_script_upload_busy() != 0u || note_all_off_requested != 0u) {
        return -3;
    }
    if (is_sample_storage_write_locked() != 0u) {
        return -3;
    }
    if (is_channel_script_active(note) == 0u) {
        return -2;
    }

    note_scale[note] = scale;
    note_amp_q15[note] = voice_amplitude_to_q15(scale);
    /* Queue only key/velocity: pitch is selected at script-controlled activation. */
    note_gate_requested[note] = 1u;
    prepare_pending_voice_stream(note, note_wave_id[note], session);
    post_voice_note_on(note, key, velocity);
    return 0;
}

int queue_voice_note(uint8_t note, uint8_t key, uint8_t velocity)
{
    return queue_voice_note_with_session(note, key, velocity, 0xFFu);
}

int queue_streamed_voice_note(uint8_t note, uint8_t key, uint8_t velocity, uint8_t session)
{
    if (session == 0xFFu) {
        return -1;
    }
    return queue_voice_note_with_session(note, key, velocity, session);
}

int queue_streamed_sample_note(uint8_t note, uint16_t sample, uint8_t key, uint8_t velocity,
                               uint8_t session)
{
    uint16_t previous;
    int result;
    if (note >= NOTE_BANK_VOICES || sample >= ATTACK_BANK_SAMPLE_COUNT) return -1;
    previous = note_wave_id[note];
    note_wave_id[note] = sample;
    result = queue_streamed_voice_note(note, key, velocity, session);
    if (result != 0) note_wave_id[note] = previous;
    return result;
}

int queue_voice_note_off(uint8_t note)
{
    if (note >= NOTE_BANK_VOICES) return -1;
    note_gate_requested[note] = 0u;
    note_freq_hz[note] = 0.0;
    note_cmd[note] = NOTE_CMD_REL;
    return 0;
}

/* ---- query voice pitch and sample assignment ---------------------------- */

uint8_t get_voice_key(uint8_t note)
{
    if (note >= NOTE_BANK_VOICES) return 0u;
    return note_next_pending[note] != 0u ? note_next_key[note] : note_play_key[note];
}

uint8_t get_voice_velocity(uint8_t note)
{
    if (note >= NOTE_BANK_VOICES) return 0u;
    return note_next_pending[note] != 0u ? note_next_velocity[note] : note_play_velocity[note];
}

double get_voice_frequency_hz(uint8_t note)
{
    return note < NOTE_BANK_VOICES ? note_freq_hz[note] : 0.0;
}

int set_voice_sample_id(uint8_t note, uint16_t wave_id)
{
    if (note >= NOTE_BANK_VOICES || wave_id >= ATTACK_BANK_SAMPLE_COUNT) {
        return -1;
    }
    note_wave_id[note] = wave_id;
    return 0;
}

uint16_t get_voice_sample_id(uint8_t note)
{
    if (note >= NOTE_BANK_VOICES) {
        return 0u;
    }
    return note_wave_id[note];
}

/* ---- query playback state ----------------------------------------------- */

uint32_t get_voice_underrun_count(void)
{
    return note_hold_miss;
}

void clear_voice_underrun_count(void)
{
    note_hold_miss = 0u;
}

uint8_t is_voice_active(uint8_t note)
{
    if (note >= NOTE_BANK_VOICES) {
        return 0u;
    }
    if (note_active[note] != 0u) {
        return 1u;
    }
    return 0u;
}

uint8_t has_active_voices(void)
{
    uint8_t i;
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        if (is_voice_active(i) != 0u) {
            return 1u;
        }
    }
    return 0u;
}

uint8_t has_voice_sample_references(void)
{
    uint8_t note;
    for (note = 0u; note < NOTE_BANK_VOICES; note++) {
        if (note_active[note] != 0u || note_next_pending[note] != 0u ||
            note_cmd[note] == NOTE_CMD_ON) {
            return 1u;
        }
    }
    return 0u;
}

/* ---- estimate body demand and remaining playback ------------------------ */

uint16_t get_voice_refill_samples_5ms(uint8_t note)
{
    uint32_t inc;
    uint64_t advance;
    if (note >= NOTE_BANK_VOICES || note_active[note] == 0u) return 0u;
    inc = note_observed_inc[note];
    if (inc < note_inc[note]) inc = note_inc[note];
    if (inc < note_inc_tgt[note]) inc = note_inc_tgt[note];
    if (inc > PHASE_INC_MAX) inc = PHASE_INC_MAX;
    advance = (uint64_t)inc * (NOTE_SAMPLE_RATE_HZ / 200u);
    if (note_body_only[note] == 0u) {
        uint64_t boundary = (uint64_t)(note_play_alen[note] > SAMPLE_CROSSFADE_LEN
                                           ? note_play_alen[note] - SAMPLE_CROSSFADE_LEN
                                           : 0u)
                            << 16;
        if (note_phase[note] < boundary) {
            uint64_t local = boundary - note_phase[note];
            if (advance <= local) return 0u;
            advance -= local;
        }
    }
    return (uint16_t)((advance + PHASE_ONE - 1u) >> 16);
}

uint32_t get_voice_buffered_time_us(uint8_t note)
{
    uint32_t inc;
    int64_t body;
    int64_t attack = 0;
    int64_t available;
    uint64_t us;
    if (note >= NOTE_BANK_VOICES || is_voice_active(note) == 0u) return 0u;
    inc = note_observed_inc[note];
    if (inc < note_inc[note]) inc = note_inc[note];
    if (inc < note_inc_tgt[note]) inc = note_inc_tgt[note];
    if (inc < PHASE_INC_MIN) inc = PHASE_INC_MIN;
    /* Reserve the second interpolation tap. Current fill excludes pending. */
    body = ((int64_t)get_current_stream_buffered_samples(note) - 1) * PHASE_ONE;
    if (note_body_only[note] != 0u || note_body_locked[note] != 0u)
        body -= (int64_t)note_body_frac[note] + (int64_t)note_body_skip[note] * PHASE_ONE;
    else {
        uint32_t fade0 = note_play_alen[note] > SAMPLE_CROSSFADE_LEN
                             ? note_play_alen[note] - SAMPLE_CROSSFADE_LEN
                             : 0u;
        body += (int64_t)fade0 * PHASE_ONE - (int64_t)note_phase[note];
    }
    if (note_body_only[note] == 0u)
        attack = ((int64_t)note_play_alen[note] - 1) * PHASE_ONE - (int64_t)note_phase[note];
    available = body > attack ? body : attack;
    if (available <= 0) return 0u;
    us = (uint64_t)available * 1000000u / ((uint64_t)inc * AUDIO_SAMPLE_RATE_HZ);
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* ---- mix the next audio sample ------------------------------------------ */

int32_t mix_next_audio_sample(void)
{
    record_stream_audio_sample();
    int64_t sum = 0;
    uint8_t i;
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        if (note_active[i] != 0u) {
            sum += (int64_t)render_voice_sample(i);
        }
    }
    return clamp_voice_mix(sum);
}

/* ---- manage script boundaries and lifecycle ----------------------------- */

void begin_voice_audio_block(void)
{
    uint8_t i;
    begin_channel_script_audio_block();
    if (note_all_off_requested != 0u) {
        /* Retire every old command/session before rendering any voice. No script
         * callback runs here; stopped ramps must not dispatch completion events. */
        for (i = 0u; i < NOTE_BANK_VOICES; i++) {
            note_cmd[i] = NOTE_CMD_NONE;
            note_gate_requested[i] = 0u;
            stop_voice_immediately(i);
        }
        reset_all_channel_script_state();
        /* LED output is latched in its driver, not in Berry's state slots. The
         * normal ramp-end handler is bypassed, so clear that output here too. */
        (void)set_channel_led_colour(0.0f, 0.0f, 0.0f, 0.0f);
        note_all_off_requested = 0u;
        return;
    }
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        process_pending_voice_command(i);
    }
}

void end_voice_audio_block(void)
{
    uint8_t i;
    for (i = 0u; i < NOTE_BANK_VOICES; i++) {
        /* Deliver the newest note command before completing its fade. */
        if (note_cmd[i] == NOTE_CMD_ON) continue;
        if (take_voice_ramp_completion(i) != 0u) {
            (void)dispatch_voice_script_handler(FW_VM_CHANNEL_HANDLER_RAMP_END, i);
        }
    }
}

void stop_voice_script(uint8_t voice)
{
    stop_channel_script(voice);
}

void stop_all_voice_scripts(void)
{
    stop_all_channel_scripts();
}

uint8_t is_voice_script_active(uint8_t voice)
{
    return is_channel_script_active(voice);
}

uint8_t get_active_voice_script_mask(void)
{
    return get_active_channel_script_mask();
}

/* ---- forward script uploads --------------------------------------------- */

int begin_voice_script_upload(uint8_t voice)
{
    if (has_active_voices() != 0u) return -2;
    return begin_channel_script_upload(voice);
}

int append_voice_script_upload_bytes(uint8_t voice, const void *data, size_t size)
{
    return append_channel_script_upload_bytes(voice, data, size);
}

int commit_voice_script_upload(uint8_t voice)
{
    int result = commit_channel_script_upload(voice);
    if (result == 0) stop_voice_immediately(voice);
    return result;
}

void abort_voice_script_upload(uint8_t voice)
{
    abort_channel_script_upload(voice);
}

uint8_t is_voice_script_upload_active(uint8_t voice)
{
    return is_channel_script_upload_active(voice);
}

uint8_t is_voice_script_upload_busy(void)
{
    return is_channel_script_upload_busy();
}

/* ---- query script diagnostics ------------------------------------------- */

const FwVmMemoryMetrics *get_voice_script_memory_metrics(void)
{
    return get_channel_script_memory_metrics();
}

FwVmFault get_voice_script_fault(uint8_t voice)
{
    return get_channel_script_fault(voice);
}

uint32_t get_voice_script_max_cycles(uint8_t voice)
{
    const FwVmMetrics *metrics = get_channel_script_metrics(voice);
    return metrics != NULL ? metrics->boundary_cycles_max : 0u;
}

uint32_t get_voice_script_fault_count(uint8_t voice)
{
    const FwVmMetrics *metrics = get_channel_script_metrics(voice);
    return metrics != NULL ? metrics->faults : 0u;
}
