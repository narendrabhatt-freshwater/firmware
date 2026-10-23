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
#include <unistd.h>
#include <vector>

using namespace std;

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
    port = ::open(RS485_DEV, O_RDWR | O_NOCTTY);
    if (port < 0) return false;
    termios mode;
    if (tcgetattr(port, &mode) < 0) {
        close();
        return false;
    }
    cfmakeraw(&mode);
    mode.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(port, TCSANOW, &mode) < 0) {
        close();
        return false;
    }

    if (!rs485_command("v")) {
        close();
        return false;
    }
    timeval timeout{0, 500};
    fd_set ready{};
    FD_SET(port, &ready);
    if (select(port + 1, &ready, nullptr, nullptr, &timeout) <= 0) {
        close();
        return false;
    }
    char version[6];
    ssize_t const count(read(port, version, sizeof version));
    if (count < 2 || version[count - 1] != '\n') {
        close();
        return false;
    }
    cerr << "version: " << version;
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
    string request(command);
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

bool voice_board_t::note_on(uint8_t channel, uint16_t sample_id,
    uint8_t key, uint8_t velocity)
{
    if (sample_id > 247) return false;
    static uint8_t const session(1);
    char command[11];
    snprintf(command, sizeof command, "n%x%02x%02x%02x%02x",
        unsigned(channel), unsigned(sample_id), unsigned(key),
        unsigned(velocity), unsigned(session));
    return rs485_command(command);
}

/* ---- release the note on one channel ------------------------------------- */

bool voice_board_t::note_off(uint8_t channel)
{
    char command[3];
    snprintf(command, 
            sizeof command, 
            "n%x", 
            unsigned(channel));
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
