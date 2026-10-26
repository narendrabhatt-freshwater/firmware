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

#include "filter.h"
#include "channel.h"

#include <cmath>

/* ---- calculate one low-pass filter's coefficients and clear its history -- */

void init_filter(filter_t& f, double cutoff_hz, double g)
{
    double const omega(2.0 * 3.14159265358979323846 * cutoff_hz / AUDIO_SAMPLE_RATE_HZ);
    double const k((4.0 * g - 3.0) / (g + 1.0));
    double p(1.0 - 0.25 * k);
    p *= p;
    double const a(1.0 / (std::tan(0.5 * omega) * (1.0 + p)));
    p = 1.0 + a;
    double const q(1.0 - a);
    double const a0(1.0 / (k + p * p * p * p));
    double const a1(4.0 * (k + p * p * p * q));
    double const a2(6.0 * (k + p * p * q * q));
    double const a3(4.0 * (k + p * q * q * q));
    double const a4(k + q * q * q * q);
    p = a0 * (k + 1.0);

    f.coef[0] = p;
    f.coef[1] = 4.0 * p;
    f.coef[2] = 6.0 * p;
    f.coef[3] = 4.0 * p;
    f.coef[4] = p;
    f.coef[5] = -a1 * a0;
    f.coef[6] = -a2 * a0;
    f.coef[7] = -a3 * a0;
    f.coef[8] = -a4 * a0;
    for (double& delay : f.d) delay = 0;
}

/* ---- filter one sample -------------------------------------------------- */

double filter_sample(filter_t& f, double in)
{
    double const out(f.coef[0] * in + f.d[0]);
    f.d[0] = f.coef[1] * in + f.coef[5] * out + f.d[1];
    f.d[1] = f.coef[2] * in + f.coef[6] * out + f.d[2];
    f.d[2] = f.coef[3] * in + f.coef[7] * out + f.d[3];
    f.d[3] = f.coef[4] * in + f.coef[8] * out;
    return out;
}
