#ifndef FILTER_H
#define FILTER_H

struct filter_t
{
    double coef[9];
    double d[4];
};

void init_filter(filter_t& f, double cutoff_hz, double g);
double filter_sample(filter_t& f, double in);

#endif
