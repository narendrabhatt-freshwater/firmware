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

#undef NDEBUG
#include <cassert>
#include "voicebd.h"
#include <options.h>
#include <RtMidi.h>
#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sysexits.h>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
char const* exe_name;
char const version[] = "0.01";
static char const product[] = "172-XXXX";
char const name[] = "Channel card voice board test";

/* ---- print usage --------------------------------------------------------- */

int usage(int status)
{
    std::ostream& out = status == EX_OK ? std::cout : std::cerr;
    out << "\nNAME\n\n"
        << "    " << exe_name << " - " << name
        << " (" << product << "). Version " << version << "\n"
        << "    (C) 2026 Freshwater Instruments\n"
        << "\nUSAGE\n\n"
        << "    " << exe_name << " [options] <sample.wav> [program.bec]\n"
        << "\nOPTIONS\n\n"
        << "    -h             Print this help and exit\n"
        << "\nARGUMENTS\n\n"
        << "    sample.wav     48 kHz, 16-bit PCM WAV, mono or stereo\n"
        << "    program.bec    BEC program (default: channel.bec in current directory)\n"
        << "\n    Uses MIDI input 0 and the device ports configured in voicebd.h.\n"
        << "    Play MIDI notes; press Ctrl+C to exit.\n\n";
    return status;
}

/* ---- handle termination signals ----------------------------------------- */

volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

/* PCM16 mono/stereo WAV; playback uses the board's default 48 kHz. */
std::vector<int16_t> read_wav(char const* path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file);
    auto read = [&](char* bytes, size_t n) { file.read(bytes, n); assert(file); };
    auto number = [&](unsigned n) {
        uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= uint32_t(uint8_t(file.get())) << (8 * i);
        assert(file);
        return value;
    };
    file.ignore(12);
    unsigned channels = 0;
    for (;;) {
        char tag[4];
        read(tag, 4);
        uint32_t const size = number(4);
        if (std::string(tag, 4) == "fmt ") {
            assert(size >= 16);
            file.ignore(2);
            channels = number(2);
            file.ignore(12);
            file.ignore(size - 16 + (size & 1));
        } else if (std::string(tag, 4) == "data") {
            assert(channels && size && size % (2 * channels) == 0);
            std::vector<int16_t> pcm(size / (2 * channels));
            for (auto& sample : pcm) {
                int sum = 0;
                for (unsigned c = 0; c < channels; ++c) {
                    int const raw = number(2);
                    sum += raw < 32768 ? raw : raw - 65536;
                }
                sample = sum / int(channels);
            }
            return pcm;
        } else file.ignore(uint64_t(size) + (size & 1));
    }
}
} // namespace

int main(int argc, char** argv)
{
    char const* const slash = std::strrchr(argv[0], '/');
    exe_name = slash ? slash + 1 : argv[0];
    bool help(false);
    opt_skip                                    /* skip over filename         */
    opt_begin(null)                             /* begin processing options   */
        option('h', help)
        default: return usage(EX_USAGE);
    opt_end
    if (help) return usage(EX_OK);
    if (argc < 1 || argc > 2) return usage(EX_USAGE);
    auto const pcm = read_wav(argv[0]);
    voice_board_config_t config;
    if (argc == 2) config.bec_file = argv[1];
    voice_board_t board;
    auto result = board.open(config);
    assert(result.ok());
    result = board.load_sample(0, pcm);
    assert(result.ok());
    RtMidiIn midi;
    midi.openPort(0);
    midi.ignoreTypes(true, true, true);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << "Ready. Play MIDI; Ctrl+C to exit.\n";
    std::array<int, 8> keys;
    keys.fill(-1);
    unsigned next = 0;
    std::vector<unsigned char> message;
    while (!stopped) {
        midi.getMessage(&message);
        if (message.size() >= 3) {
            unsigned const type = message[0] & 0xf0;
            int const key = ((message[0] & 15) << 8) | message[1];
            if (type == 0x90 && message[2]) {
                unsigned voice = next;
                for (unsigned i = 0; i < 8; ++i)
                    if (keys[(next + i) % 8] < 0) { voice = (next + i) % 8; break; }
                result = board.note_on(voice, 0, message[1], message[2]);
                assert(result.ok());
                keys[voice] = key;
                next = (voice + 1) % 8;
            } else if (type == 0x80 || (type == 0x90 && !message[2])) {
                for (unsigned i = 0; i < 8; ++i)
                    if (keys[i] == key) {
                        result = board.note_off(i);
                        if (!result.ok()) std::cerr << "note_off: " << result.message << '\n';
                        assert(result.ok());
                        keys[i] = -1;
                    }
            }
        } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    board.close();
}
