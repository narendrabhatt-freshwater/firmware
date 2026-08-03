/**
 ******************************************************************************
 * @file    audio_rate.h
 * @brief   Single source of truth for Channel Card audio sample rate.
 *
 * I2S AudioFreq, CS4304 CLK_CFG_1, note-bank / tone phase increments, and
 * filter coefficient design must all use this value (see
 * audio-dsp-conventions.mdc). Change here + CS4304_SAMPLE_RATE_* + Cube
 * I2S AudioFreq together.
 ******************************************************************************
 */

#ifndef __AUDIO_RATE_H__
#define __AUDIO_RATE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** System sample rate (Hz). Boss bench target: 96 kHz. */
#define AUDIO_SAMPLE_RATE_HZ 96000u

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_RATE_H__ */
