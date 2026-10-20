#ifndef _CHANNEL_H_
#define _CHANNEL_H_

#include <cstdint>
#include <vector>

#define AUDIO_SAMPLE_RATE_HZ 48000u
#define SPB 48

void generate_samples(int32_t* buffer);
void received_serial(std::vector<uint8_t> const&);
void received_usb(std::vector<uint8_t> const&);
void write_serial(std::vector<uint8_t> const&);
void write_usb(std::vector<uint8_t> const&);

#endif

