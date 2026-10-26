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

#include "../channel-rewrite/voicebd.h"
#include "../channel-rewrite/devices.h"

#include <RtMidi.h>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

using namespace std;

static volatile sig_atomic_t stopped(0);
static constexpr unsigned voice_count(8);
static constexpr uint8_t sample_id(0);

/* ---- show the connection check and MIDI input options ---------------------- */

static int usage(char const* name, int status)
{
    ostream& out(status ? cerr : cout);
    out << "Usage: " << name << " [MIDI_PORT]\n"
        << "       " << name << " --check\n"
        << "       " << name << " --list\n"
        << "MIDI_PORT defaults to 0. Uses sample 0 and eight channel voices.\n"
        << "Play MIDI notes; Ctrl+C sends all notes off and exits.\n";
    return status;
}

/* ---- request shutdown from the main loop --------------------------------- */

static void stop(int)
{ stopped = 1; }

/* ---- report an unsuccessful serial write --------------------------------- */

static void check_write(bool result)
{ if (!result) throw runtime_error("RS485 write failed"); }

/* ---- check the channel connection or send MIDI notes -------------------- */

int main(int argc, char** argv)
try
{
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))
        return usage(argv[0], 0);
    if (argc > 2) return usage(argv[0], 1);
    voice_board_t board;
    if (argc == 2 && !strcmp(argv[1], "--check")) {
        cout << "RS485: " << RS485_DEV << '\n' << flush;
        if (!board.open()) {
            cerr << "Voice-board connection/version check failed.\n";
            return 1;
        }
        cout << "Version check passed.\n";
        return 0;
    }
    RtMidiIn midi;
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (unsigned i(0); i < midi.getPortCount(); ++i)
            cout << "MIDI " << i << ": " << midi.getPortName(i) << '\n';
        if (!midi.getPortCount()) cout << "No MIDI inputs found.\n";
        return 0;
    }
    unsigned long input(0);
    if (argc == 2) {
        char* end;
        input = strtoul(argv[1], &end, 10);
        if (!*argv[1] || *end) return usage(argv[0], 1);
    }
    if (input >= midi.getPortCount()) {
        cerr << "MIDI input " << input << " is unavailable. Use --list.\n";
        return 1;
    }
    if (!board.open()) {
        cerr << "Voice-board connection/version check failed: " << RS485_DEV << '\n';
        return 1;
    }
    midi.ignoreTypes(true, true, true);
    midi.openPort(unsigned(input));
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    cout << "MIDI " << input << ": " << midi.getPortName(unsigned(input)) << '\n'
         << "RS485: " << RS485_DEV << '\n'
         << "Ready. Play MIDI notes; Ctrl+C exits.\n" << flush;

    array<int, voice_count> keys;
    keys.fill(-1);
    unsigned next(0);
    vector<unsigned char> message;
    while (!stopped) {
        midi.getMessage(&message);
        if (message.empty()) {
            usleep(1000);
            continue;
        }
        if (message.size() < 3) continue;
        unsigned const type(message[0] & 0xf0);
        int const key(((message[0] & 0x0f) << 8) | message[1]);
        if (type == 0x90 && message[2]) {
            unsigned voice(next);
            for (unsigned i(0); i < voice_count; ++i) {
                unsigned const candidate((next + i) % voice_count);
                if (keys[candidate] < 0) {
                    voice = candidate;
                    break;
                }
            }
            check_write(board.note_on(uint8_t(voice), sample_id,
                message[1], message[2]));
            keys[voice] = key;
            next = (voice + 1) % voice_count;
        } else if (type == 0x80 || (type == 0x90 && !message[2])) {
            for (unsigned i(0); i < voice_count; ++i) {
                if (keys[i] == key) {
                    check_write(board.note_off(uint8_t(i)));
                    keys[i] = -1;
                }
            }
        }
    }
    check_write(board.all_notes_off());
    board.close();
    return 0;
}
catch (exception const& error)
{
    cerr << error.what() << '\n';
    return 1;
}
