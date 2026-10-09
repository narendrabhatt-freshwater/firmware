#ifndef CHANNEL_APP_AUDIO_H
#define CHANNEL_APP_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- set the audio sample rate ------------------------------------------ */

/* Keep I2S AudioFreq, CS4304 clock settings, and filter
 * coefficient design consistent with this rate. */
#define AUDIO_SAMPLE_RATE_HZ 48000u

/* ---- control audio playback --------------------------------------------- */

/* Start both I2S outputs independently of USB enumeration. CH1 carries the
 * note-bank mix; CH2–CH4 carry calibrated DC outputs. Set their targets
 * with Audio_SetDCLevel() before starting playback. */
void Audio_StartPlayback(void);

/* Set calibrated DC output for channel 2..4, percent -100..100.
 * The output slews toward the target, including during startup. */
void Audio_SetDCLevel(uint8_t channel, int8_t percent);

/* Service the I2S2 underrun guard from the TIM7 interrupt. */
void Audio_I2S2_Pump(void);

/* ---- query audio diagnostics -------------------------------------------- */

uint32_t Audio_Bridge_UsbDropCount(void);
void Audio_Bridge_UsbDropCountClear(void);
uint32_t Audio_Bridge_MaxFill(void);
uint32_t Audio_Bridge_FillLate(void);

/* LED_Y is low throughout each SPI1 DMA refill and high while idle. */
void Audio_Bridge_CpuLoadProbeSet(uint8_t enabled);
uint8_t Audio_Bridge_CpuLoadProbeGet(void);

#ifdef __cplusplus
}
#endif

#endif
