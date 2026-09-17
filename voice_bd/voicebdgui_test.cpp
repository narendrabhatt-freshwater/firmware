#define VOICEBD_GUI_TEST
#include "voicebdgui.cpp"

using namespace gui;
void require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejected(F f) {
    bool caught = false;
    try { f(); } catch (std::exception const&) { caught = true; }
    require(caught, "Invalid input accepted");
}
struct fake_board {
    std::vector<std::array<unsigned, 4>> on;
    std::vector<unsigned> off;
    voice_board_result_t all_notes_off() { return {}; }
    voice_board_result_t note_off(uint8_t v) { off.push_back(v); return {}; }
    voice_board_result_t note_on(uint8_t v, uint16_t s, uint8_t k, uint8_t vel) {
        on.push_back({v, s, k, vel}); return {};
    }
};
int main() try {
    std::vector<uint8_t> vc(0x5500, 128);
    std::fill(vc.begin(), vc.begin() + 0x1500, 0);
    vc[0] = 4; vc[1] = 4;
    for (unsigned i = 0; i < 16384; ++i) vc[0x1500 + i] = uint8_t(i);
    auto data = decode_vc(vc, 1);
    require(data.samples.size() == 16384, "One-shot length");
    require(data.samples[0] == -1.0f / 128 && data.samples[127] == -1.0f &&
        data.samples[128] == 127.0f / 128, "VC segment ordering/unsigned conversion");
    vc[0x1332] = 1; vc[0x1333] = 2; vc[0x133b] = 1;
    data = decode_vc(vc, 1);
    require(data.samples.size() == 14080 && data.samples[384] == data.samples[128] &&
        data.samples[639] == data.samples[383], "Inclusive loop endpoints");
    vc[0x1400] = 1; vc[0x1401] = 0x12; vc[0x1404] = 2;
    require(decode_vc(vc, 1).samples[0] == data.samples[256], "Embedded STSEG");
    vc[0x1404] = 128; rejected([&] { decode_vc(vc, 1); }); vc[0x1404] = 0;
    vc[0x1332] = 10; rejected([&] { decode_vc(vc, 1); }); vc[0x1332] = 1;
    vc[0] = 139; rejected([&] { decode_vc(vc, 1); }); vc[0] = 4;
    vc.resize(0x5400); rejected([&] { decode_vc(vc, 1); });
    vc.resize(0x5500); vc[1] = 0;
    require(decode_vc(vc, 1).samples[256] == decode_vc(vc, 1).samples[128], "Revision-0 one-segment loop migration");
    vc[1] = 2; vc[0x133b] = 2;
    require(decode_vc(vc, 1).samples.size() == 16384, "Revision-2 invalid loop flag migration");
    sound silence{{0, 0, 0, 0}, 14080, {}};
    auto samples = pcm(silence);
    require(samples.size() == 14 && std::all_of(samples.begin(), samples.end(), [](auto n) { return n == 0; }), "Resampling silence");
    sound dc{std::vector<float>(14080, .5f), 14080, {}};
    for (unsigned bank = 0; bank < 6; ++bank) {
        auto bank_pcm = pcm(dc, 48000u >> bank);
        require(bank_pcm.size() == (48000u >> bank) && bank_pcm.front() == 16383 && bank_pcm.back() == 16383, "Bank rate/amplitude");
    }
    // A real, minimal stereo WAV: downmix the first frame to zero, second to .5.
    std::vector<uint8_t> wav(52, 0);
    auto tag = [&](unsigned p, char const* s) { std::copy(s, s + 4, wav.begin() + p); };
    auto u16 = [&](unsigned p, unsigned n) { wav[p] = n & 255; wav[p + 1] = n >> 8; };
    tag(0, "RIFF"); wav[4] = 44; tag(8, "WAVE"); tag(12, "fmt "); wav[16] = 16;
    u16(20, 1); u16(22, 2); u16(24, 48000); u16(32, 4); u16(34, 16);
    tag(36, "data"); wav[40] = 8; u16(44, 16384); u16(46, 49152); u16(48, 16384); u16(50, 16384);
    auto stereo = decode_wav(wav);
    require(stereo.samples == std::vector<float>({0, .5f}) && stereo.rate == 48000, "WAV stereo conversion");
    wav[34] = 24; rejected([&] { decode_wav(wav); }); wav[34] = 16;
    wav[40] = 255; rejected([&] { decode_wav(wav); });
    for (size_t size = 0; size < 52; ++size) rejected([&] { decode_wav(std::vector<uint8_t>(wav.begin(), wav.begin() + size)); });
    fake_board board; voices<fake_board> notes{board};
    notes.message({0x90, 60, 100}); notes.message({0x91, 60, 100});
    notes.message({0x80, 60, 0}); require(board.off == std::vector<unsigned>{0}, "Channel-specific release");
    notes.message({0xb1, 64, 127}); notes.message({0x91, 60, 0});
    require(board.off.size() == 1, "Sustain released prematurely");
    notes.message({0xb1, 64, 0}); require(board.off.back() == 1, "Sustain pedal release");
    notes.stop(); board.on.clear(); board.off.clear();
    for (uint8_t k = 60; k < 69; ++k) notes.message({0x90, k, 100});
    require(board.on.size() == 9 && board.on.back()[0] == 0, "Oldest voice steal");
    notes.message({0x80, 60, 0}); require(board.off.empty(), "Stolen key released new owner");
    notes.message({0x90, 127, 100});
    require(board.on.back()[1] == 5 && board.on.back()[2] == 67, "Upper octave bank selection");
    notes.message({0xb0, 123, 0}); require(board.off.size() == 8, "MIDI all notes off");
    notes.stop(); notes.message({0x90, 60, 80}); notes.message({0x90, 60, 90});
    notes.message({0x80, 60, 0}); require(notes.slots[0].key == -1 && notes.slots[1].held, "Repeated-note ownership");
    std::cout << "GUI decoder, resampler and MIDI tests passed\n";
    return 0;
} catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
