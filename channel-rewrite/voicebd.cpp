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

#include "voicebd.h"
#include "devices.h"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define CHANNEL_NUMBER 1

using namespace std;

/* ---- read the monotonic clock in microseconds ---------------------------- */

static int64_t monotonic_microseconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return int64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

/* ---- create a disconnected voice board ----------------------------------- */

voice_board_t::voice_board_t() : port(-1)
{}

/* ---- release the connection when the board leaves scope ------------------ */

voice_board_t::~voice_board_t()
{ close(); }

/* --- open the voice board serial port and check for the version reply ---- */

bool voice_board_t::open()
{
    close();
    port = ::open(RS485_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port < 0) return false;
    termios mode;
    if (tcgetattr(port, &mode) < 0) {
        close();
        return false;
    }
    cfmakeraw(&mode);
    mode.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    mode.c_cflag |= CS8 | CLOCAL | CREAD;
    mode.c_cc[VMIN] = 0;
    mode.c_cc[VTIME] = 0;
#ifdef __APPLE__
    cfsetspeed(&mode, 921600);
#else
    cfsetspeed(&mode, B921600);
#endif
    if (tcsetattr(port, TCSANOW, &mode) < 0) {
        close();
        return false;
    }
    if (fcntl(port, F_SETFL, 0) < 0) {
        close();
        return false;
    }

    int64_t const deadline(monotonic_microseconds() + 1500);
    if (!rs485_command("v")) {
        close();
        return false;
    }
    char version[5];
    size_t received(0);
    fd_set ready{};
    FD_SET(port, &ready);
    while (received < sizeof version) {
        timeval timeout{0, static_cast<suseconds_t>(deadline - monotonic_microseconds())};
        if (timeout.tv_usec <= 0) break;
        int const result(select(port + 1, &ready, nullptr, nullptr, &timeout));
        if (result <= 0) break;
        ssize_t const count(read(port, version + received, sizeof version - received));
        if (count <= 0) break;
        received += size_t(count);
    }
    if (received != sizeof version || version[4] != '\n') {
        close();
        return false;
    }
    cerr << "version: ";
    cerr.write(version, received);
    return true;
}

/* ---- close the voice board serial port ---------------------------------- */

void voice_board_t::close()
{
    if (port >= 0) ::close(port);
    port = -1;
}

/* ---- send a complete command without waiting for a reply ---------------- */

bool voice_board_t::rs485_command(char const* command)
{
    string request(to_string(CHANNEL_NUMBER));
    request += command;
    request.push_back('\r');
    size_t sent(0);
    while (sent < request.size()) {
        ssize_t const count(write(port, request.data() + sent,
                            request.size() - sent));
        if (count <= 0) return false;
        sent += size_t(count);
    }
    return true;
}

/* ---- send a note with fixed-width hexadecimal fields --------------------- */

bool voice_board_t::note_on(uint8_t voice, uint16_t sample_id,
    uint8_t key, uint8_t velocity)
{
    if (sample_id > 247) return false;
    static uint8_t const session(1);
    char command[11];
    snprintf(command, sizeof command, "n%x%02x%02x%02x%02x",
        unsigned(voice), unsigned(sample_id), unsigned(key),
        unsigned(velocity), unsigned(session));
    return rs485_command(command);
}

/* ---- release the note on one voice --------------------------------------- */

bool voice_board_t::note_off(uint8_t voice)
{
    char command[3];
    snprintf(command, 
            sizeof command, 
            "n%x", 
            unsigned(voice));
    return rs485_command(command);
}

/* ---- release all notes --------------------------------------------------- */

bool voice_board_t::all_notes_off()
{ return rs485_command("n"); }

/* -------- load sample ------------------------------------------------------*/

bool voice_board_t::load_sample(uint16_t sample_id,
                                std::vector<int16_t>const& pcm)
{
    // TODO: HANDLE SAMPLE LOADING
    return true;
}

/* -------- load script ------------------------------------------------------*/

bool voice_board_t::load_script(uint8_t channel, std::string const& path)
{
    // TODO: LOAD SCRIPT
    return true;
}
