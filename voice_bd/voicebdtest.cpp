#undef NDEBUG
#include <cassert>
#include "voicebd.h"
#include <RtMidi.h>
#include <array>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
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
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <sample.wav> [program.bec]\n";
        return 1;
    }
    auto const pcm = read_wav(argv[1]);
    voice_board_config_t config;
    if (argc == 3) config.bec_file = argv[2];
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
