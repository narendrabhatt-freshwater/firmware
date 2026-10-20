#ifndef TEST_AUDIO_CONSUMER_H
#define TEST_AUDIO_CONSUMER_H
#include "channel.h"

/* ---- render one shared mono block --------------------------------------- */

static void next_mono_samples(int32_t *output)
{
    generate_samples(output);
}
#endif
