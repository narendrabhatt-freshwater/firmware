#ifndef NOTE_ENVELOPE_H
#define NOTE_ENVELOPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOTE_ENV_VOICE_COUNT 8u

/* Native sample-rate amplitude ramp used by the Channel VM adapter. Musical
 * envelope policy and section sequencing belong exclusively to VM programs. */
void NoteEnv_Init(void);
void NoteEnv_Stop(uint8_t voice);
int NoteEnv_SetAmplitude(uint8_t voice, float amplitude);
int NoteEnv_StartRamp(uint8_t voice, float target, float slope);
float NoteEnv_Amplitude(uint8_t voice);
float NoteEnv_RenderSample(uint8_t voice);
uint8_t NoteEnv_TakeRampEnd(uint8_t voice);

#ifdef __cplusplus
}
#endif

#endif
