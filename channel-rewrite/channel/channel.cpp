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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <assert.h>

#define FIRMWARE_VERSION "0001"

using namespace std;

/* ---- global variables ---------------------------------------------------- */

static uint32_t milliseconds;
static bool echo(false);

/* ---- generate the next 48 samples for DAC -------------------------------- */

void generate_samples(int32_t* buffer)
{
    milliseconds++;
    constexpr double tau = 6.28318530717958647692;
    constexpr double step = tau * 440.0 / AUDIO_SAMPLE_RATE_HZ;
    constexpr double amplitude = INT32_MAX / 4.0;
    static double s = 0;
    static double c = 1;
    static double const ds = std::sin(step);
    static double const dc = std::cos(step);
    for (int i(0); i < SPB; ++i) {
        buffer[i] = static_cast<int32_t>(s * amplitude);
        double const next_s = s * dc + c * ds;
        c = c * dc - s * ds;
        s = next_s;
    }
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
        cerr << "version: " FIRMWARE_VERSION "\n";
    } else if (command == "n") {
        std::cerr << "all notes off\n";
    }
    else if (command.size() == 2 &&
        std::sscanf(command.c_str(), "n%1x", &channel) == 1)
    {
        std::cerr << "note off: " << channel << '\n';
    }
    else if (command.size() == 10 &&
        std::sscanf(command.c_str(), "n%1x%2x%2x%2x%2x",
            &channel, &sample, &key, &velocity, &session) == 5)
    {
        std::cerr << "note on: " << command << '\n';
    }
    else if (command.size() == 5 &&
        std::sscanf(command.c_str(), "a%4x", &gain) == 1)
    {
        std::cerr << "attenuator: " << command << '\n';
    } else if (command == "r") {
        rs485_response("ok");
        cerr << "reset" << endl;
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
            cerr << "command " << cmd_buffer << endl;
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

/* ---- handle USB responses ------------------------------------------------ */

void received_usb(vector<uint8_t> const&)
{
    // TODO
}
