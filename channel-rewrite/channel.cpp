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

#include "channel.h"
#include "hardware.h"
#include "oscillator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <assert.h>

#define FIRMWARE_VERSION "0001"
#define CHANNEL_COUNT 8

using namespace std;

/* ---- global variables ---------------------------------------------------- */

static uint32_t milliseconds;
static bool echo(false);

struct oscillator_t
{
    double position;
    double step;
    double level;
};

static oscillator_t oscillators[CHANNEL_COUNT]{};

/* ---- start the oscillator ------------------------------------------------*/

static void note_on(uint8_t channel, uint8_t key)
{
    oscillator_t& oscillator(oscillators[channel]);
    oscillator.position = 0;
    oscillator.step =
        440.0 * pow(2.0, (key - 69.0) / 12.0) * 512 / AUDIO_SAMPLE_RATE_HZ;
    oscillator.level = 0.25;
}

/* ---- stop the oscillator ------------------------------------------------ */
static void note_off(uint8_t channel)
{
    oscillators[channel].level = 0;
}

/* ---- generate one oscillator sample ------------------------------------- */

static int32_t oscillator_sample(oscillator_t& oscillator)
{
    if (oscillator.level == 0) return 0;
    int32_t const sample(wavetable[int(oscillator.position)]);
    oscillator.position += oscillator.step;
    if (oscillator.position >= 512) oscillator.position -= 512;
    return static_cast<int32_t>(sample * oscillator.level);
}

/* ---- mix one sample from all oscillators -------------------------------- */

static int32_t mix_oscillators()
{
    double sample(0);
    for (oscillator_t& oscillator : oscillators)
        sample += oscillator_sample(oscillator);
    return static_cast<int32_t>(clamp(sample,
                                      double(INT32_MIN),
                                      double(INT32_MAX)));
}

/* ---- generate the next 48 samples for DAC -------------------------------- */

void generate_samples(int32_t* buffer)
{
    milliseconds++;
    for (int i(0); i < SPB; ++i) buffer[i] = mix_oscillators();
}

/* ---- write a response to RS485 ------------------------------------------- */

static void rs485_response(string const& s)
{
    vector<uint8_t> response(s.begin(), s.end());
    if (echo) response.push_back('\r');
    response.push_back('\n');
    write_serial(response);
}

/* ---- parse an RS485 command and send its defined reply ------------------- */

static void process_command(string const& command)
{
    assert(!command.empty());
    unsigned int channel;
    unsigned int sample;
    unsigned int key;
    unsigned int velocity;
    unsigned int session;
    unsigned int gain;

    if (command == "v") {
        rs485_response(FIRMWARE_VERSION);
        fprintf(stderr, "version: " FIRMWARE_VERSION "\n");
    } else if (command == "n") {
        for (uint8_t channel(0); channel < CHANNEL_COUNT; ++channel) 
            note_off(channel);
        fprintf(stderr, "all notes off\n");
    }
    else if (command.size() == 2 &&
        std::sscanf(command.c_str(), "n%1x", &channel) == 1 &&
        channel < CHANNEL_COUNT)
    {
        note_off(channel);
        fprintf(stderr, "note off: %u\n", channel);
    }
    else if (command.size() == 10 &&
        std::sscanf(command.c_str(), "n%1x%2x%2x%2x%2x",
            &channel, &sample, &key, &velocity, &session) == 5 &&
        channel < CHANNEL_COUNT && key < 128)
    {
        note_on(channel, key);
        fprintf(stderr, "note on: %s\n", command.c_str());
    }
    else if (command.size() == 5 &&
        std::sscanf(command.c_str(), "a%4x", &gain) == 1)
    {
        fprintf(stderr, "attenuator: %s\n", command.c_str());
    } else if (command == "r") {
        rs485_response("ok");
        fprintf(stderr, "reset\n");
    } else if (command == "e1") {
        echo = true;
        rs485_response("ok"); 
    } else if (command == "e0") {
        echo = false;
    } // TODO more commands here... 
}

/* ---- accumulate RS485 bytes until newline ------------------------------- */

void received_serial(vector<uint8_t> const& bytes)
{
    static string cmd_buffer;
    for (uint8_t byte : bytes) {
        if (byte == '\r') {
            // cerr << "command " << cmd_buffer << endl;
            if (echo) rs485_response(string());
            if (!cmd_buffer.empty()) {
                process_command(cmd_buffer);
                cmd_buffer.clear();
            }
        } else if (byte == '\b' || byte == 0x7f) {
            if (!cmd_buffer.empty()) {
                cmd_buffer.pop_back();
                if (echo) write_serial({'\b', ' ', '\b'});
            }
        } else { 
            if (echo) write_serial({byte});
            cmd_buffer.push_back(char(byte)); 
        }
    }
}
