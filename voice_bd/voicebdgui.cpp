// Standalone Channel Card sound browser. No dependency on MAS or an audio API.
#include "voicebd.h"
#include <RtMidi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <pthread.h>
#endif

namespace gui {
namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
void check(voice_board_result_t const& r) { if (!r) throw std::runtime_error(r.message); }
std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string printable(std::string s) {
    for (auto& c : s) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    return s;
}

struct sound {
    std::vector<float> samples;
    unsigned rate = 14080;
    std::string detail;
};

std::vector<uint8_t> read_file(fs::path const& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + path.string());
    auto size = f.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024)
        throw std::runtime_error("Empty file or file exceeds 64 MiB");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Incomplete file read");
    return bytes;
}

sound decode_vc(std::vector<uint8_t> const& b, unsigned loop_seconds) {
    // S2XP2SRC/LOADF.SA: 128-byte sectors, descriptors at LSN $22,
    // internal control patch at $28, waveform at $2a, EOF at $aa.
    constexpr size_t waveform = 0x1500, descriptor = 0x1100 + 35 * 16;
    if (b.size() != 0x5500) throw std::runtime_error("VC must contain exactly 21760 bytes");
    if (b[0] < 1 || b[0] > 6 || b[1] > 4)
        throw std::runtime_error("Invalid VC mode/revision header");
    if (loop_seconds < 1 || loop_seconds > 120) throw std::runtime_error("Invalid loop duration");
    unsigned start = 0, loop_begin = b[descriptor + 2], loop_end = b[descriptor + 3];
    bool loop = b[descriptor + 11] != 0;
    // Match LOADF's OLDREV migration of pre-revision-3 loop descriptors.
    if (b[1] < 2) {
        loop = loop_begin < 128 && loop_end < 128 && loop_end >= loop_begin;
        if (loop && loop_end - loop_begin == 1) loop_end = loop_begin;
    } else if (b[1] == 2) loop = b[descriptor + 11] == 1;
    // Revision 4 embeds .CO; older files name an external .CO, which this
    // waveform audition tool does not execute. MAS patch_parameter: STSEG=$12.
    if (b[1] == 4) for (size_t p = 0x1400; p < 0x1500 && b[p]; p += 8) {
        if (b[p + 1] == 0x12) { start = unsigned(b[p + 3]) * 256 + b[p + 4]; break; }
    }
    if (start >= 128) throw std::runtime_error("VC start segment is outside its waveform");
    if (loop && (loop_begin >= 128 || loop_end >= 128 || loop_end < loop_begin))
        throw std::runtime_error("VC loop is outside its waveform");
    sound result;
    size_t const first = start * 128, last = loop ? (loop_end + 1) * 128 : 16384;
    bool const looping = loop && first < last;
    size_t const count = looping ? size_t(14080) * loop_seconds : 16384 - first;
    result.samples.reserve(count);
    for (size_t i = first; result.samples.size() < count; ++i) {
        if (looping && i == last) i = loop_begin * 128;
        // Match MAS q_cpu_t::wrwave(): each 128-byte disk segment is reversed
        // when copied into the cached channel waveform.
        size_t const disk = waveform + (i / 128) * 128 + (127 - i % 128);
        result.samples.push_back((int(b[disk]) - 128) / 128.0f);
    }
    result.detail = "VC mode " + std::to_string(b[0]) + " rev " + std::to_string(b[1]) +
        " | 14080 Hz | start " + std::to_string(start) +
        (looping ? " | loop " + std::to_string(loop_begin) + ".." + std::to_string(loop_end) +
                   " rendered " + std::to_string(loop_seconds) + "s" : " | one shot");
    if (b[0] <= 3) result.detail += " | waveform only (no mode-1 synthesis)";
    return result;
}

sound decode_wav(std::vector<uint8_t> const& b) {
    auto tag = [&](size_t p, char const* t) {
        return p + 4 <= b.size() && std::equal(b.begin() + p, b.begin() + p + 4, t);
    };
    auto u16 = [&](size_t p) { return unsigned(b.at(p)) | unsigned(b.at(p + 1)) << 8; };
    auto u32 = [&](size_t p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; };
    if (b.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE"))
        throw std::runtime_error("Not a RIFF/WAVE file");
    uint64_t const end = uint64_t(u32(4)) + 8;
    if (end > b.size() || end < 12) throw std::runtime_error("Truncated WAV");
    unsigned channels = 0, rate = 0;
    size_t data = 0, length = 0;
    for (size_t p = 12; p + 8 <= end;) {
        uint32_t const n = u32(p + 4);
        if (uint64_t(p) + 8 + n + (n & 1) > end) throw std::runtime_error("Truncated WAV chunk");
        if (tag(p, "fmt ")) {
            if (n < 16 || u16(p + 8) != 1 || u16(p + 22) != 16)
                throw std::runtime_error("WAV must be signed 16-bit PCM");
            channels = u16(p + 10); rate = u32(p + 12);
            if ((channels != 1 && channels != 2) || rate < 1000 || rate > 192000 ||
                u16(p + 20) != channels * 2) throw std::runtime_error("Unsupported WAV format");
        } else if (tag(p, "data")) { data = p + 8; length = n; }
        p += 8 + size_t(n) + (n & 1);
    }
    if (!channels || !data || !length || length % (channels * 2))
        throw std::runtime_error("Missing or invalid WAV sample data");
    sound result; result.rate = rate;
    result.samples.reserve(length / (channels * 2));
    for (size_t p = data; p < data + length;) {
        int sum = 0;
        for (unsigned c = 0; c < channels; ++c, p += 2) {
            int value = int(u16(p)); sum += value < 32768 ? value : value - 65536;
        }
        result.samples.push_back(float(sum) / (32768.0f * channels));
    }
    result.detail = "WAV | " + std::to_string(rate) + " Hz | " + std::to_string(channels) + " channel(s)";
    return result;
}

sound load_sound(fs::path const& path, unsigned seconds) {
    auto bytes = read_file(path);
    auto ext = lower(path.extension().string());
    if (ext == ".vc") return decode_vc(bytes, seconds);
    if (ext == ".wav") return decode_wav(bytes);
    throw std::runtime_error("Select a .VC or .wav file");
}

// MAS stable_resample_t's four-point Hermite interpolation. Work is done once
// during loading, never in the transport's refill loop.
std::vector<int16_t> pcm(sound const& input, unsigned output_rate = 48000) {
    size_t const count = size_t(double(input.samples.size()) * output_rate / input.rate + 0.5);
    if (input.samples.empty() || !count) throw std::runtime_error("No samples to play");
    std::vector<int16_t> output(count);
    auto sample = [&](int64_t i) { return input.samples[size_t(std::clamp<int64_t>(i, 0, input.samples.size() - 1))]; };
    for (size_t n = 0; n < count; ++n) {
        double const pos = double(n) * input.rate / output_rate;
        int64_t const i = int64_t(pos); float const f = float(pos - i);
        float const xm1 = sample(i - 1), x0 = sample(i), x1 = sample(i + 1), x2 = sample(i + 2);
        float const c = (x1 - xm1) * .5f, v = x0 - x1, w = c + v;
        float const a = w + v + (x2 - x0) * .5f, b = w + a;
        output[n] = int16_t(std::clamp((((a * f) - b) * f + c) * f + x0, -1.0f, 1.0f) * 32767.0f);
    }
    return output;
}

std::vector<fs::path> scan(fs::path const& root) {
    if (!fs::is_directory(root)) throw std::runtime_error("Sound library is not a directory: " + root.string());
    std::vector<fs::path> files;
    for (auto const& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        auto ext = lower(entry.path().extension().string());
        if ((ext == ".vc" || ext == ".wav") && entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) { return lower(a.string()) < lower(b.string()); });
    return files;
}

// Keep ownership by channel/key; release the oldest repeated note, and steal
// the oldest occupied voice only after all eight voices have been used.
template<class Board> struct voices {
    struct slot { int key = -1; bool held = false; uint64_t age = 0; };
    Board& board;
    std::array<slot, 8> slots{};
    std::array<bool, 16> sustain{};
    uint64_t age = 0;
    void reset() { slots = {}; sustain = {}; }
    void stop() { check(board.all_notes_off()); reset(); }
    void release(unsigned i) { check(board.note_off(uint8_t(i))); slots[i] = {}; }
    void message(std::vector<unsigned char> const& m) {
        if (m.size() < 3 || m[1] > 127 || m[2] > 127) return;
        unsigned const type = m[0] & 0xf0, channel = m[0] & 15;
        int const key = int(channel * 128 + m[1]);
        if (type == 0x90 && m[2]) {
            unsigned chosen = 0;
            for (unsigned i = 0; i < slots.size(); ++i) {
                if (slots[i].key < 0) { chosen = i; break; }
                if (slots[i].age < slots[chosen].age) chosen = i;
            }
            // Like MAS's octave banks: prevent the firmware's >2x octave fold
            // from making the upper MIDI octaves play the wrong pitch.
            unsigned note = m[1], bank = 0;
            while (note > 72) { note -= 12; ++bank; }
            check(board.note_on(uint8_t(chosen), uint16_t(bank), uint8_t(note), m[2]));
            slots[chosen] = {key, true, ++age};
        } else if (type == 0x80 || (type == 0x90 && !m[2])) {
            unsigned chosen = 8;
            for (unsigned i = 0; i < slots.size(); ++i)
                if (slots[i].key == key && slots[i].held && (chosen == 8 || slots[i].age < slots[chosen].age)) chosen = i;
            if (chosen != 8) { slots[chosen].held = false; if (!sustain[channel]) release(chosen); }
        } else if (type == 0xb0) {
            if (m[1] == 64 || m[1] == 121) {
                sustain[channel] = m[1] == 64 && m[2] >= 64;
                if (!sustain[channel]) for (unsigned i = 0; i < 8; ++i)
                    if (slots[i].key / 128 == int(channel) && slots[i].key >= 0 && !slots[i].held) release(i);
            } else if (m[1] == 120 || m[1] == 123) {
                for (unsigned i = 0; i < 8; ++i) if (slots[i].key >= 0 && slots[i].key / 128 == int(channel)) release(i);
                sustain[channel] = false;
            }
        }
    }
};

struct options {
    voice_board_config_t board;
    fs::path library;
    unsigned midi_port = MIDI_PORT, loop_seconds = 30;
    bool browse = false, list_midi = false;
    fs::path check_file;
    bool check_library = false;
};

struct player {
    struct state { std::string status = "Initializing...", loaded, detail, midi; unsigned active = 0; bool ready = false; };
    enum action { load, panic };
    struct command { action type; fs::path path; };
    options config;
    std::mutex mutex;
    state view;
    std::deque<command> commands;
    std::atomic<bool> quitting{false};
    std::thread worker;
    explicit player(options const& o) : config(o), worker([this] { run(); }) {}
    ~player() { quitting = true; if (worker.joinable()) worker.join(); }
    state snapshot() { std::lock_guard<std::mutex> l(mutex); return view; }
    void status(std::string text) { std::lock_guard<std::mutex> l(mutex); view.status = std::move(text); }
    void submit(action type, fs::path path = {}) {
        std::lock_guard<std::mutex> l(mutex);
        commands.clear(); // Keep the latest requested selection; never build an upload backlog.
        commands.push_back({type, std::move(path)});
    }
    void run() {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        voice_board_t board;
        voices<voice_board_t> notes{board};
        try {
            std::unique_ptr<RtMidiIn> midi;
            if (!config.browse) {
                midi = std::make_unique<RtMidiIn>();
                if (config.midi_port >= midi->getPortCount()) throw std::runtime_error("MIDI port unavailable; use --list-midi or --midi-port N");
                auto name = midi->getPortName(config.midi_port);
                midi->ignoreTypes(true, true, true);
                midi->openPort(config.midi_port);
                { std::lock_guard<std::mutex> l(mutex); view.midi = name; }
                check(board.open(config.board)); // Opens CDC + RS485 and initializes all eight voices.
            }
            { std::lock_guard<std::mutex> l(mutex); view.ready = true; }
            status(config.browse ? "Browse only: no hardware or MIDI connected" : "Ready. Select a sound and press L.");
            bool loaded = false;
            std::vector<unsigned char> message;
            auto drain = [&] { if (midi) do { midi->getMessage(&message); } while (!message.empty()); };
            while (!quitting) {
                command cmd{}; bool have_command = false;
                { std::lock_guard<std::mutex> l(mutex);
                    if (!commands.empty()) { cmd = commands.front(); commands.pop_front(); have_command = true; }
                }
                try {
                    if (have_command && cmd.type == panic) {
                        if (!config.browse) notes.stop();
                        drain(); status("All notes stopped.");
                    } else if (have_command) {
                        if (!config.browse) notes.stop();
                        status("Loading " + cmd.path.filename().string() + "...");
                        // Disable notes until every bank succeeds; partial uploads must not
                        // expose a mixture of old and new instruments to MIDI.
                        loaded = false;
                        { std::lock_guard<std::mutex> l(mutex); view.loaded.clear(); view.detail.clear(); }
                        auto sound = load_sound(cmd.path, config.loop_seconds);
                        for (unsigned bank = 0; bank < 6 && !quitting; ++bank) {
                            auto samples = pcm(sound, 48000u >> bank);
                            if (!config.browse) check(board.load_sample(uint16_t(bank), samples));
                        }
                        drain();
                        loaded = !quitting;
                        { std::lock_guard<std::mutex> l(mutex); view.loaded = cmd.path.filename().string(); view.detail = sound.detail; }
                        status(config.browse ? "Decoded successfully (browse only)." : "Loaded. Play your MIDI keyboard.");
                    }
                    if (midi) for (unsigned n = 0; n < 256 && !quitting; ++n) {
                        midi->getMessage(&message);
                        if (message.empty()) break;
                        if (loaded) notes.message(message);
                    }
                } catch (std::exception const& e) {
                    loaded = false;
                    if (!config.browse) { (void)board.all_notes_off(); notes.reset(); }
                    drain(); status(std::string("Error: ") + e.what() + " | Press L to retry.");
                    std::lock_guard<std::mutex> l(mutex); view.loaded.clear(); view.detail.clear();
                }
                { std::lock_guard<std::mutex> l(mutex);
                    view.active = unsigned(std::count_if(notes.slots.begin(), notes.slots.end(), [](auto const& s) { return s.key >= 0; }));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (midi) midi->closePort();
        } catch (RtMidiError const& e) {
            status("MIDI error: " + e.getMessage() + " | Quit and reconnect the MIDI input.");
        } catch (std::exception const& e) { status(std::string("Initialization failed: ") + e.what()); }
        if (board.is_open()) (void)board.close();
    }
};

enum key { up = 256, down, page_up, page_down, home, end };
struct terminal {
    termios saved{};
    std::string pending;
    clock_type::time_point escape_at{};
    terminal() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &saved))
            throw std::runtime_error("Run voicebdgui in an interactive terminal");
        auto raw = saved; raw.c_lflag &= ~(ICANON | ECHO); raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) throw std::runtime_error("Cannot configure terminal");
        std::cout << "\033[?1049h\033[?25l" << std::flush;
    }
    ~terminal() { std::cout << "\033[0m\033[?25h\033[?1049l" << std::flush; tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
    std::pair<unsigned, unsigned> size() const {
        winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        return {ws.ws_row ? ws.ws_row : 24, ws.ws_col ? ws.ws_col : 80};
    }
    int read_key() {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&fd, 1, pending.empty() ? 20 : 1) > 0) {
            if (fd.revents & (POLLHUP | POLLERR)) { interrupted = 1; return -1; }
            char bytes[128]; auto n = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (n > 0) { if (pending.empty()) escape_at = clock_type::now(); pending.append(bytes, size_t(n)); }
        }
        if (pending.empty()) return -1;
        if (pending[0] != '\033') { int c = static_cast<unsigned char>(pending[0]); pending.erase(0, 1); return c; }
        static std::pair<char const*, int> const codes[] = {
            {"\033[A", up}, {"\033[B", down}, {"\033[5~", page_up}, {"\033[6~", page_down},
            {"\033[H", home}, {"\033[F", end}, {"\033OH", home}, {"\033OF", end}};
        for (auto const& code : codes) {
            std::string seq = code.first;
            if (pending.compare(0, seq.size(), seq) == 0) { pending.erase(0, seq.size()); return code.second; }
        }
        if (clock_type::now() - escape_at < std::chrono::milliseconds(35)) return -1;
        // Consume unknown control sequences instead of interpreting their tail as commands.
        if (pending.size() > 1 && (pending[1] == '[' || pending[1] == 'O')) {
            size_t n = 2; while (n < pending.size() && !(pending[n] >= '@' && pending[n] <= '~')) ++n;
            pending.erase(0, std::min(n + 1, pending.size())); return -1;
        }
        pending.erase(0, 1); return 27;
    }
};

struct browser {
    std::vector<fs::path> files;
    std::vector<std::string> names;
    std::vector<size_t> visible;
    std::string query;
    size_t selected = 0;
    bool searching = false;
    browser(fs::path const& root) : files(scan(root)) {
        for (auto const& p : files) names.push_back(p.lexically_relative(root).string());
        filter();
    }
    void filter() {
        visible.clear(); auto q = lower(query);
        for (size_t i = 0; i < names.size(); ++i) if (lower(names[i]).find(q) != std::string::npos) visible.push_back(i);
        selected = 0;
    }
    void move(int amount) { if (!visible.empty()) selected = size_t(std::clamp<int64_t>(int64_t(selected) + amount, 0, visible.size() - 1)); }
};

void browse(options const& config) {
    browser files(config.library);
    terminal term;
    player engine(config);
    std::string previous;
    while (!interrupted) {
        auto [rows, cols] = term.size();
        unsigned const page = rows > 11 ? rows - 11 : 1;
        int const c = term.read_key();
        if (files.searching) {
            if (c == 27 || c == '\n' || c == '\r') files.searching = false;
            else if (c == 127 || c == 8) { if (!files.query.empty()) files.query.pop_back(); files.filter(); }
            else if (c == 21) { files.query.clear(); files.filter(); }
            else if (c >= 32 && c < 127 && files.query.size() < 200) { files.query += char(c); files.filter(); }
        } else if (c == 'q' || c == 'Q' || c == 4) break;
        else if (c == '/') files.searching = true;
        else if (c == 27) { files.query.clear(); files.filter(); }
        else if (c == ' ') engine.submit(player::panic);
        else if ((c == 'l' || c == 'L' || c == '\n' || c == '\r') && !files.visible.empty())
            engine.submit(player::load, files.files[files.visible[files.selected]]);
        if (c == up) files.move(-1);
        if (c == down) files.move(1);
        if (c == page_up) files.move(-int(page));
        if (c == page_down) files.move(int(page));
        if (c == home) files.selected = 0;
        if (c == end && !files.visible.empty()) files.selected = files.visible.size() - 1;
        auto state = engine.snapshot();
        std::vector<std::string> lines = {
            "FRESHWATER  |  Channel Card sound browser",
            "Library: " + config.library.string(),
            "USB: " + config.board.usb_port + "   RS485: " + config.board.rs485_port,
            "MIDI: " + (config.browse ? std::string("disabled (browse only)") : state.midi) + "   Voices: " + std::to_string(state.active) + "/8",
            std::string(files.searching ? "Search (Enter finishes): " : "Filter: ") + files.query +
                "   [" + std::to_string(files.visible.size()) + "/" + std::to_string(files.files.size()) + "]",
            ""};
        size_t const first = files.selected / page * page;
        for (size_t i = first; i < first + page; ++i)
            lines.push_back(i < files.visible.size() ? std::string(i == files.selected ? "> " : "  ") + files.names[files.visible[i]] : "");
        lines.push_back("Loaded: " + (state.loaded.empty() ? std::string("none") : state.loaded));
        lines.push_back(state.detail);
        lines.push_back(state.status);
        lines.push_back("Arrows/PgUp/PgDn: select   /: search   Enter: finish search   L: load");
        lines.push_back("Space: stop notes   Esc: clear filter   Q / Ctrl+C: stop and quit");
        std::ostringstream screen;
        screen << "\033[H";
        for (size_t i = 0; i < std::min<size_t>(rows, lines.size()); ++i) {
            screen << "\033[2K" << printable(lines[i]).substr(0, cols > 1 ? cols - 1 : 1);
            if (i + 1 < rows) screen << "\r\n";
        }
        screen << "\033[J";
        auto text = screen.str();
        if (text != previous) { std::cout << text << std::flush; previous = std::move(text); }
    }
}

unsigned number(std::string const& value, unsigned max) {
    if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected a nonnegative integer: " + value);
    auto n = std::stoul(value);
    if (n > max) throw std::runtime_error("Option value out of range: " + value);
    return unsigned(n);
}

fs::path executable_directory(char const* arg0) {
#ifdef __APPLE__
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0) return fs::canonical(path.data()).parent_path();
#elif defined(__linux__)
    std::error_code error;
    auto path = fs::read_symlink("/proc/self/exe", error);
    if (!error) return path.parent_path();
#endif
    return fs::absolute(arg0).parent_path();
}
} // namespace gui

#ifndef VOICEBD_GUI_TEST
int main(int argc, char** argv) try {
    using namespace gui;
    options config;
    char const* home_dir = std::getenv("HOME");
    config.library = fs::path(home_dir ? home_dir : ".") / "cmi-local/soundlib";
    config.board.bec_file = (executable_directory(argv[0]) / "series2.bec").string();
    config.board.initial_attenuation_db = 18;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error("Missing value for " + arg); return argv[i]; };
        if (arg == "--help" || arg == "-h") {
            std::cout << "voicebdgui [--library DIR] [--usb-port PORT] [--rs485-port PORT]\n"
                "           [--midi-port N] [--bec FILE] [--attenuation DB] [--loop-seconds N]\n"
                "           [--browse-only | --list-midi | --check FILE | --check-library]\n"
                "Defaults: ~/cmi-local/soundlib, series2.bec beside executable, 18 dB attenuation,\n"
                "30 seconds of rendered VC loops. / searches, Enter ends search, L loads.\n"
                "Arrows select, Space stops, Q/Ctrl+C quits. VC controls/envelopes are not emulated.\n";
            return 0;
        } else if (arg == "--library") config.library = fs::absolute(value());
        else if (arg == "--usb-port") config.board.usb_port = value();
        else if (arg == "--rs485-port") config.board.rs485_port = value();
        else if (arg == "--bec") config.board.bec_file = value();
        else if (arg == "--midi-port") config.midi_port = number(value(), 1024);
        else if (arg == "--attenuation") config.board.initial_attenuation_db = uint8_t(number(value(), 127));
        else if (arg == "--loop-seconds") { config.loop_seconds = number(value(), 120); if (!config.loop_seconds) throw std::runtime_error("Loop duration must be positive"); }
        else if (arg == "--browse-only") config.browse = true;
        else if (arg == "--list-midi") config.list_midi = true;
        else if (arg == "--check") config.check_file = value();
        else if (arg == "--check-library") config.check_library = true;
        else throw std::runtime_error("Unknown option: " + arg);
    }
    if (config.list_midi) {
        RtMidiIn midi;
        for (unsigned i = 0; i < midi.getPortCount(); ++i) std::cout << i << ": " << midi.getPortName(i) << '\n';
        if (!midi.getPortCount()) std::cout << "No MIDI inputs found.\n";
        return 0;
    }
    if (!config.check_file.empty()) {
        auto data = load_sound(config.check_file, config.loop_seconds);
        auto samples = pcm(data);
        std::cout << data.detail << "\n48 kHz PCM: " << samples.size() << " samples\n";
        return 0;
    }
    if (config.check_library) {
        unsigned passed = 0, failed = 0;
        for (auto const& p : scan(config.library)) try { (void)load_sound(p, 1); ++passed; }
            catch (std::exception const& e) { ++failed; std::cout << p.string() << ": " << e.what() << '\n'; }
        std::cout << passed << " decoded, " << failed << " rejected\n";
        return failed ? 1 : 0;
    }
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt); std::signal(SIGHUP, interrupt);
    browse(config);
    return 0;
} catch (RtMidiError const& e) { std::cerr << "voicebdgui: " << e.getMessage() << '\n'; return 1;
} catch (std::exception const& e) { std::cerr << "voicebdgui: " << e.what() << '\n'; return 1; }
#endif
