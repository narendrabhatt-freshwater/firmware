#ifndef CHANNEL_LED_H
#define CHANNEL_LED_H

#include <stdint.h>

void ChannelLed_Init(void);
int ChannelLed_Set(float red, float green, float blue, float brightness);
void ChannelLed_Task(void);

#endif
