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
#include <RtMidi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <exception>
#include <sysexits.h>
#include <fstream>
#include <filesystem>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
char const* exe_name;
char const version[] = "0.01";
static char const product[] = "172-XXXX";
char const name[] = "Channel card voice board test";
// Change to 2..8 for polyphony; 1 sends every press/release directly to voice 0.
constexpr unsigned num_voices = 8;
static_assert(num_voices >= 1 && num_voices <= voice_board_t::voice_count,
              "num_voices must be between 1 and 8");
constexpr uint32_t sample_rate_hz = 48000;
constexpr double sample_root_hz = 261.625565; // C4, MIDI key 60.

/* Nominal BODY demand; a custom card program can change the playback pitch. */
void print_key_demand(unsigned voice, unsigned key)
{
    static char const* const notes[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    double const hz = 440.0 * std::exp2((int(key) - 69) / 12.0);
    double const speed = std::clamp(hz / sample_root_hz, 1.0 / 16.0, 16.0);
    std::ostringstream line;
    line << "key=" << key << " (" << notes[key % 12] << int(key / 12) - 1
         << ") voice=" << voice << " required=" << std::fixed << std::setprecision(2)
         << sample_rate_hz / 1000.0 * speed << " samples/ms\n";
    std::cout << line.str() << std::flush;
}

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
        << "    --usb-port PORT     Override the Channel CDC port\n"
        << "    --rs485-port PORT   Override the RS485 adapter port\n"
        << "\nARGUMENTS\n\n"
        << "    sample.wav     48 kHz, 16-bit PCM WAV, mono or stereo\n"
        << "    program.bec    Firmware program (default: series2.bec in current directory)\n"
        << "\n    Uses MIDI input 0 and the device ports configured in voicebd.h.\n"
        << "    Initializes the voice program on all eight voices when opening the board.\n"
        << "    MIDI voices: " << num_voices << " (1 = direct mono, 2..8 = polyphony).\n"
        << "    O: orchestra, S: sine, P: square, T: triangle, W: sawtooth; R: compile/upload series2.be; Space: hard stop/reset; Ctrl+C exits.\n\n";
    return status;
}

/* ---- handle termination signals ----------------------------------------- */

volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

/* Read terminal keys immediately, restoring the terminal on normal exit/errors.
 * Keep ISIG enabled so Ctrl+C still goes through the termination handler. */
struct terminal_keys {
    termios saved{};
    bool changed = false;
    terminal_keys() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios mode = saved;
        mode.c_lflag &= ~(ICANON | ECHO);
        changed = tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
    }
    ~terminal_keys() {
        if (changed) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    int read_key() const {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        char key;
        return poll(&input, 1, 0) > 0 && (input.revents & POLLIN) &&
            read(STDIN_FILENO, &key, 1) == 1 ? key : -1;
    }
};

/* Assumes a valid PCM16 mono/stereo WAV; convert its rate to 48 kHz. */
std::vector<int16_t> read_wav(char const* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open WAV: " + std::string(path));
    file.exceptions(std::ios::failbit | std::ios::badbit);
    auto number = [&](unsigned n) {
        uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= uint32_t(uint8_t(file.get())) << (8 * i);
        return value;
    };
    file.ignore(12);
    unsigned channels = 0;
    uint32_t rate = sample_rate_hz;
    for (;;) {
        char tag[4];
        file.read(tag, 4);
        uint32_t const size = number(4);
        if (std::string(tag, 4) == "fmt ") {
            file.ignore(2);
            channels = number(2);
            rate = number(4);
            file.ignore(8);
            file.ignore(uint64_t(size) - 16 + (size & 1));
        } else if (std::string(tag, 4) == "data") {
            std::vector<int16_t> pcm(size / (2 * channels));
            for (auto& sample : pcm) {
                int sum = 0;
                for (unsigned c = 0; c < channels; ++c) {
                    int const raw = number(2);
                    sum += raw < 32768 ? raw : raw - 65536;
                }
                sample = sum / int(channels);
            }
            if (rate == 0) throw std::runtime_error("invalid WAV sample rate");
            if (rate == sample_rate_hz || pcm.empty()) return pcm;
            std::vector<int16_t> converted(
                uint64_t(pcm.size()) * sample_rate_hz / rate);
            for (size_t i = 0; i < converted.size(); ++i) {
                double const position = double(i) * rate / sample_rate_hz;
                size_t const at = static_cast<size_t>(position);
                double const fraction = position - at;
                converted[i] = static_cast<int16_t>(std::lround(
                    pcm[at] * (1.0 - fraction) +
                    pcm[std::min(at + 1, pcm.size() - 1)] * fraction));
            }
            return converted;
        } else file.ignore(uint64_t(size) + (size & 1));
    }
}
} // namespace

int main(int argc, char** argv)
try
{
    char const* const slash = std::strrchr(argv[0], '/');
    exe_name = slash ? slash + 1 : argv[0];
    auto const base_path = std::filesystem::path(argv[0]).parent_path();
    auto const orchestra_path =
        base_path / "orch01.vc_SV001.wav";
    auto const sine_path = base_path / "sample.wav";
    auto const square_path = base_path / "square.wav";
    auto const sawtooth_path = base_path / "sawtooth.wav";
    auto const triangle_path = base_path / "triangle.wav";
    // Keep argument parsing local; all host transport code lives in voicebd.cpp.
    voice_board_config_t config;
    int positional=0;
    for (int i=1;i<argc;++i) {
        if (std::strcmp(argv[i],"-h")==0 || std::strcmp(argv[i],"--help")==0) return usage(EX_OK);
        if (std::strcmp(argv[i],"--usb-port")==0 || std::strcmp(argv[i],"--rs485-port")==0) {
            bool usb=std::strcmp(argv[i],"--usb-port")==0;
            if(++i==argc) return usage(EX_USAGE);
            (usb ? config.usb_port : config.rs485_port)=argv[i]; continue;
        }
        if (argv[i][0]=='-') return usage(EX_USAGE);
        argv[positional++]=argv[i];
    }
    argc=positional;
    if (argc < 1 || argc > 2) return usage(EX_USAGE);
    auto const pcm = read_wav(argv[0]);
    if (argc == 2) config.bec_file = argv[1];
    auto check = [](voice_board_result_t const& result) {
        if (!result) throw std::runtime_error(result.message);
    };
    voice_board_t board;
    check(board.open(config));
    check(board.load_sample(0, pcm));
    RtMidiIn midi;
    midi.ignoreTypes(true, true, true);
    if (midi.getPortCount() == 0)
        throw std::runtime_error("No MIDI input ports found");
    midi.openPort(0);
    std::cout << "MIDI 0: " << midi.getPortName(0) << '\n';
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << "Ready. MIDI voices: " << num_voices
              << "; Space stops all notes/reset state; R compiles/uploads series2.be; Ctrl+C to exit.\n";
    terminal_keys keyboard;
    // Used only in polyphonic mode: MIDI channel/key owning each voice.
    std::array<int, num_voices> keys;
    keys.fill(-1);
    unsigned next = 0;
    std::vector<unsigned char> message;
    while (!stopped) {
        try {
            int const keypress = keyboard.read_key();
            switch (keypress) {
                case 'r':
                case 'R': {
                    check(board.all_notes_off());
                    keys.fill(-1);
                    next = 0;
                    std::cout << "Compiling series2.be..." << std::endl;
#if defined(__linux__) && defined(__aarch64__)
                    char const* command = "./berry.linux-arm64 series2.be -o series2.bec";
#else
                    char const* command = "./berry series2.be -o series2.bec";
#endif
                    if (std::system(command) == 0) {
                        for (uint8_t voice = 0; voice < voice_board_t::voice_count; ++voice)
                            check(board.load_script(voice, "series2.bec"));
                        std::cout << "series2.bec loaded into voices 0-7." << std::endl;
                    } else {
                        std::cerr << "Compilation failed; no program uploaded.\n";
                    }
                    do { midi.getMessage(&message); } while (!message.empty());
                    break;
                }
                case ' ': {
                    check(board.all_notes_off());
                    keys.fill(-1);
                    next = 0;
                    std::cout << "All notes stopped; Berry state reset.\n";
                    break;
                }
                case 'o':
                case 'O': {
                    std::cout << "Loading orchestral sample into sample 0...\n";
                    auto const replacement = read_wav(orchestra_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 's':
                case 'S': {
                    std::cout << "Loading sine wave sample into sample 0...\n";
                    auto const replacement = read_wav(sine_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 'p':
                case 'P': {
                    std::cout << "Loading square wave sample into sample 0...\n";
                    auto const replacement = read_wav(square_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 't':
                case 'T': {
                    std::cout << "Loading triangle wave sample into sample 0...\n";
                    auto const replacement = read_wav(triangle_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                case 'w':
                case 'W': {
                    std::cout << "Loading sawtooth wave sample into sample 0...\n";
                    auto const replacement = read_wav(sawtooth_path.c_str());
                    check(board.load_sample(0, replacement, sample_rate_hz, sample_root_hz));
                    break;
                }
                default:
                    break;
            }

            midi.getMessage(&message);
            if (message.size() >= 3) {
                unsigned const type = message[0] & 0xf0;
                if constexpr (num_voices == 1) {
                    // No key tracking: even releases after Space reach the board.
                    if (type == 0x90 && message[2]) {
                        check(board.note_on(0, 0, message[1], message[2]));
                        print_key_demand(0, message[1]);
                    } else if (type == 0x80 || (type == 0x90 && !message[2])) {
                        check(board.note_off(0));
                    }
                } else {
                    int const key = ((message[0] & 15) << 8) | message[1];
                    if (type == 0x90 && message[2]) {
                        unsigned voice = next;
                        // Prefer an unassigned voice; otherwise steal in round-robin order.
                        for (unsigned i = 0; i < num_voices; ++i) {
                            unsigned const candidate = (next + i) % num_voices;
                            if (keys[candidate] < 0) { voice = candidate; break; }
                        }
                        next = (voice + 1) % num_voices;
                        keys[voice] = -1;
                        check(board.note_on(voice, 0, message[1], message[2]));
                        print_key_demand(voice, message[1]);
                        keys[voice] = key;
                    } else if (type == 0x80 || (type == 0x90 && !message[2])) {
                        for (unsigned voice = 0; voice < num_voices; ++voice) {
                            if (keys[voice] == key) {
                                keys[voice] = -1;
                                check(board.note_off(voice));
                            }
                        }
                    }
                }
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (std::exception const& error) {
            std::cerr << exe_name << ": " << error.what()
                      << "; fix series2.be and press R to reload.\n";
        }
    }
    board.close();
}
catch (std::exception const& error) {
    std::cerr << exe_name << ": " << error.what() << '\n';
    return EX_SOFTWARE;
}
