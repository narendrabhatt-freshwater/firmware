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

#ifndef CHANNEL_AUDIO_OUTPUT_H
#define CHANNEL_AUDIO_OUTPUT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Output-backend interface. The application supplies mono samples; these
 * functions own calibrated DAC slots and physical playback. */
void start_audio_playback(void);
void service_i2s2_underrun(void);
void set_control_voltage_target(uint8_t channel, int8_t percent);
int32_t render_next_control_voltage_sample(uint8_t ch);
void render_control_voltage_channels(int32_t *buf, uint32_t frames, uint8_t left, uint8_t right);
void render_control_voltage_slot(int32_t *buf, uint32_t frames, uint8_t ch, uint8_t slot);
void render_dac_audio_frames(int32_t *buf, uint32_t frames);
#ifdef __cplusplus
}
#endif
#endif
