/* Real host streaming/credit worker against the simulator's production PTYs.
 * Including the implementation exposes diagnostics without a second client
 * stealing replies from the active RS485 connection. */
#include "voicebd.cpp"
#include <iostream>
#include <cmath>
using namespace voice_board_detail;
static void require_result(voice_board_result_t result) {
    if (!result) { std::cerr << result.message << std::endl; std::exit(1); }
}
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    device_context state;
    voice_board_config_t config;
    config.bec_file = argv[1]; config.usb_port = argv[2]; config.rs485_port = argv[3];
    unsigned duration = (unsigned)std::stoul(argv[4]);
    require_result(open_device(&state, config));
    std::vector<int16_t> pcm(48000 * (duration * 4 + 10));
    for (size_t i=0; i<pcm.size(); ++i) pcm[i]=(int16_t)(1000 * sin(i * 0.025));
    require_result(load_sample(&state, 0, pcm, 261.625565));
    struct Pattern { unsigned voices, key; bool retrigger; };
    for (auto pattern : {Pattern{1,60,false}, Pattern{8,60,false},
                         Pattern{8,72,false}, Pattern{8,60,true}}) {
        unsigned voices = pattern.voices;
        {
            device_context::control_turn turn(&state);
            require_result(state.rs485.command("usb 0"));
        }
        for (unsigned i=0; i<voices; ++i)
            require_result(note_on(&state, i, 0, pattern.key, 127));
        if (pattern.retrigger) {
            // An explicit musical pattern, not a recovery delay.
            for (unsigned step=0; step<duration*20; ++step) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                require_result(note_on(&state, step % voices, 0, 60 + step % 13, 100));
            }
        } else std::this_thread::sleep_for(std::chrono::seconds(duration));
        if (!device_is_open(&state)) { std::cerr << "stream connection failed" << std::endl; return 1; }
        {
            device_context::control_turn turn(&state);
            auto stats = state.rs485.command("usb"); require_result(stats);
            std::cout << voices << " voices, key " << pattern.key
                      << (pattern.retrigger ? ", retrigger: " : ": ") << stats.message << std::endl;
            if (stats.message.find("drop 0 hold 0 ") == std::string::npos)
                return 1;
        }
        require_result(all_notes_off(&state));
        // This interval is the test's release phase, not a transport workaround.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    // MAS stores four pitch banks per instrument (index + 0/8/16/24).
    // Exercise that caller contract with finite samples, overlapping notes,
    // oldest-channel replacement, release, and a stopped patch reload.
    for (unsigned bank = 0; bank < 4; ++bank) {
        std::vector<int16_t> wave(48000u >> bank);
        for (size_t i = 0; i < wave.size(); ++i)
            wave[i] = int16_t(1000 * sin(i * 0.025 * (1u << bank)));
        for (unsigned instrument = 0; instrument < 8; ++instrument)
            require_result(load_sample(&state, instrument + 8 * bank, wave, 261.625565));
    }
    {
        device_context::control_turn turn(&state);
        require_result(state.rs485.command("usb 0"));
    }
    for (unsigned step = 0; step < 64; ++step) {
        unsigned channel = step % 8;
        unsigned bank = (step / 8) % 4;
        require_result(note_on(&state, channel, channel + 8 * bank,
                               60 + step % 13, 1 + step % 127));
        if (step % 3 == 0) require_result(note_off(&state, channel));
        if (step % 8 == 7) {
            // Chord interval belongs to the score, not transport recovery.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    require_result(all_notes_off(&state));
    for (unsigned channel = 0; channel < 8; ++channel)
        require_result(load_script(&state, channel, argv[1]));
    for (unsigned channel = 0; channel < 8; ++channel)
        require_result(note_on(&state, channel, channel + 24, 72, 127));
    // Run past these finite samples, then release and restart the sequencer.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (unsigned channel = 0; channel < 8; ++channel)
        require_result(note_off(&state, channel));
    require_result(all_notes_off(&state));
    require_result(note_on(&state, 0, 0, 60, 127));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        device_context::control_turn turn(&state);
        auto stats = state.rs485.command("usb"); require_result(stats);
        std::cout << "MAS bank/sequence/reload pattern: " << stats.message << std::endl;
        if (stats.message.find("drop 0 hold 0 ") == std::string::npos) return 1;
    }
    require_result(close_device(&state));
}
