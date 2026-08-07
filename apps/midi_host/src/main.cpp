/**
 * @file main.cpp
 * @brief midi_host — MIDI in → 16-voice FIFO bank → speakers and/or Channel Card.
 *
 * MIDI: any device via --midi N; auto-pick prefers Launchkey "MIDI Out".
 * Output: speakers (default), or Channel Card note bank over RS485 (--target channel).
 */

#include "audio_engine.hpp"
#include "auto_player.hpp"
#include "channel_rs485.hpp"
#include "midi_input.hpp"
#include "note_event.hpp"
#include "pitch.hpp"
#include "voice_bank.hpp"

#include "host_io/rs485/bus.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#include <sys/select.h>
#endif

using midi_host::AudioEngine;
using midi_host::AutoPlayer;
using midi_host::BankEvent;
using midi_host::BankEventKind;
using midi_host::ChannelRs485Out;
using midi_host::MidiInput;
using midi_host::MidiPortInfo;
using midi_host::NoteAction;
using midi_host::NoteEvent;
using midi_host::VoiceBank;

namespace
{

/** Set by SIGINT/SIGTERM so Ctrl+C still runs Close(). */
std::atomic<bool> g_quit_signal{false};

#if !defined(_WIN32)
void OnQuitSignal(int /*sig*/)
{
  g_quit_signal.store(true);
}
#endif

enum class OutTarget
{
  Speaker,
  Channel,
};

enum class PlayMode
{
  Live,
  Auto,
};

void PrintUsage()
{
  std::cout
      << "midi_host - MIDI → 16-voice FIFO bank → speakers or Channel Card\n"
         "\n"
         "Usage:\n"
         "  midi_host [--midi N] [--auto]                # speakers (default)\n"
         "  midi_host --target channel --rs485 PATH [--midi N] [--gain DB] [--auto]\n"
         "            [--echo-off|--echo-on|--echo-leave]\n"
         "  midi_host --list\n"
         "  midi_host -h | --help\n"
         "\n"
         "MIDI input (any controller; Launchkey is only the auto-pick default):\n"
         "  --midi N, --port N   MIDI input port index (see --list)\n"
         "  --list               List MIDI input ports and exit\n"
         "  (default)            Prefer Launchkey \"… MIDI Out\" (not DAW Out)\n"
         "\n"
         "Play mode:\n"
         "  (default)            Live MIDI only — wait for keys\n"
         "  --auto               Looping Am demo until first Note On, then live MIDI\n"
         "\n"
         "Output:\n"
         "  (default)            Play sines on the Mac default speakers\n"
         "  --target channel     Drive Channel Card N0–NF over RS485 (no local speaker)\n"
         "  --rs485 PATH         USB↔RS485 adapter serial path (required with channel)\n"
         "  --baud N             RS485 baud (default 460800)\n"
         "  --gain DB            CH1 DAC atten in dB at session start (0..127, default 6)\n"
         "  --echo-off           Send e:ec 0 at open (default)\n"
         "  --echo-on            Send e:ec 1 at open\n"
         "  --echo-leave         Do not change Effect echo\n"
         "\n"
         "Velocity is ignored. Up to 16 notes; overflow steals the oldest.\n"
         "Duplicate Note On retriggers the same oscillator slot.\n"
         "Press Enter or Ctrl-C to quit.\n";
}

const char* KindName(BankEventKind kind)
{
  switch (kind) {
  case BankEventKind::On:
    return "ON";
  case BankEventKind::Off:
    return "OFF";
  case BankEventKind::Steal:
    return "STEAL";
  case BankEventKind::Retrig:
    return "RETRIG";
  }
  return "?";
}

void PrintBankEvent(const BankEvent& ev, bool via_rs485)
{
  const std::string name = midi_host::MidiNoteName(ev.midi_key);
  /* [midi] = VoiceBank only. [C]ok is printed later by ChannelRs485Out. */
  std::printf("%-6s slot=%-2u note=%-3u %-3s  freq=%7.2f Hz  active=%u/16%s\n",
              KindName(ev.kind),
              static_cast<unsigned>(ev.slot),
              static_cast<unsigned>(ev.midi_key),
              name.c_str(),
              ev.freq_hz,
              static_cast<unsigned>(ev.active_count),
              via_rs485 ? "  [midi]" : "");
  // No fflush — under a 16-key mash, flushing every line stalls the MIDI
  // poll loop and delays RS485 Offs (notes hang on the card).
}

void ApplyBankEvents(const std::vector<BankEvent>& events,
                     AudioEngine* audio,
                     ChannelRs485Out* channel,
                     VoiceBank& bank)
{
  for (const BankEvent& ev : events) {
    /* RS485 / speakers first — never stall the bus behind terminal I/O. */
    if (audio) {
      audio->ApplyBankEvent(ev);
    }
    if (channel) {
      channel->ApplyBankEvent(ev, bank);
    }
    PrintBankEvent(ev, channel != nullptr);
  }
}

#if !defined(_WIN32)
bool StdinReady()
{
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}

/**
 * Quit on Enter/q without blocking the MIDI loop.
 * Never call getline() here — if any keystroke arrives, getline would wait
 * for a full line and freeze note handling for seconds.
 */
bool ConsumeQuitRequest()
{
  if (g_quit_signal.load()) {
    return true;
  }
  if (!StdinReady()) {
    return false;
  }
  char buf[64];
  const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) {
    return false;
  }
  for (ssize_t i = 0; i < n; ++i) {
    if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == 'q' || buf[i] == 'Q') {
      return true;
    }
  }
  return false;
}
#else
bool ConsumeQuitRequest()
{
  return g_quit_signal.load();
}
#endif

bool LooksLikeSerialPath(const char* s)
{
  return std::strncmp(s, "/dev/", 5) == 0 || std::strncmp(s, "cu.", 3) == 0 ||
         std::strncmp(s, "tty.", 4) == 0 || std::strncmp(s, "COM", 3) == 0;
}

} // namespace

int main(int argc, char** argv)
{
#if !defined(_WIN32)
  // Prefer a clean Close() over abrupt exit.
  // until the host clears it.
  std::signal(SIGINT, OnQuitSignal);
  std::signal(SIGTERM, OnQuitSignal);
#endif
  bool list_only = false;
  bool auto_play = false;
  std::optional<unsigned> midi_index;
  OutTarget out_target = OutTarget::Speaker;
  std::string rs485_path;
  uint32_t rs485_baud = 460800;
  uint32_t gain_db = 6;
  host_io::rs485::EffectEcho effect_echo = host_io::rs485::EffectEcho::Off;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      PrintUsage();
      return 0;
    }
    if (std::strcmp(arg, "--list") == 0) {
      list_only = true;
      continue;
    }
    if (std::strcmp(arg, "--auto") == 0) {
      auto_play = true;
      continue;
    }
    if (std::strcmp(arg, "--echo-off") == 0) {
      effect_echo = host_io::rs485::EffectEcho::Off;
      continue;
    }
    if (std::strcmp(arg, "--echo-on") == 0) {
      effect_echo = host_io::rs485::EffectEcho::On;
      continue;
    }
    if (std::strcmp(arg, "--echo-leave") == 0) {
      effect_echo = host_io::rs485::EffectEcho::Leave;
      continue;
    }
    if (std::strcmp(arg, "--midi") == 0 || std::strcmp(arg, "--port") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "err: " << arg << " requires a value\n";
        return 1;
      }
      const char* val = argv[++i];
      // --port can be MIDI index (numeric) or, with --target channel, an
      // RS485 path — matches fw midi channel --port /dev/... style.
      if (LooksLikeSerialPath(val)) {
        rs485_path = val;
        if (rs485_path.rfind("cu.", 0) == 0 || rs485_path.rfind("tty.", 0) == 0) {
          rs485_path = std::string("/dev/") + rs485_path;
        }
        continue;
      }
      char* end = nullptr;
      const long v = std::strtol(val, &end, 10);
      if (end == val || *end != '\0' || v < 0) {
        std::cerr << "err: invalid MIDI port index (use a number, or a "
                     "/dev/... path for RS485)\n";
        return 1;
      }
      midi_index = static_cast<unsigned>(v);
      continue;
    }
    if (std::strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "err: --target requires speaker|channel\n";
        return 1;
      }
      const char* t = argv[++i];
      if (std::strcmp(t, "speaker") == 0) {
        out_target = OutTarget::Speaker;
      } else if (std::strcmp(t, "channel") == 0) {
        out_target = OutTarget::Channel;
      } else {
        std::cerr << "err: --target must be speaker or channel\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(arg, "--rs485") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "err: --rs485 requires a serial path\n";
        return 1;
      }
      rs485_path = argv[++i];
      continue;
    }
    if (std::strcmp(arg, "--baud") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "err: --baud requires a value\n";
        return 1;
      }
      char* end = nullptr;
      const long v = std::strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || v <= 0) {
        std::cerr << "err: invalid --baud\n";
        return 1;
      }
      rs485_baud = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(arg, "--gain") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "err: --gain requires dB 0..127\n";
        return 1;
      }
      char* end = nullptr;
      const long v = std::strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || v < 0 || v > 127) {
        std::cerr << "err: --gain must be 0..127\n";
        return 1;
      }
      gain_db = static_cast<uint32_t>(v);
      continue;
    }
    // Bare "channel" as positional (fw midi channel …).
    if (std::strcmp(arg, "channel") == 0) {
      out_target = OutTarget::Channel;
      continue;
    }
    if (std::strcmp(arg, "speaker") == 0) {
      out_target = OutTarget::Speaker;
      continue;
    }
    std::cerr << "err: unknown argument: " << arg << "\n";
    PrintUsage();
    return 1;
  }

  if (list_only) {
    try {
      const std::vector<MidiPortInfo> ports = MidiInput::ListPorts();
      if (ports.empty()) {
        std::cout << "(no MIDI input ports)\n";
        return 0;
      }
      for (const MidiPortInfo& p : ports) {
        std::cout << p.index << ": " << p.name << "\n";
      }
    } catch (const std::exception& ex) {
      std::cerr << "err: " << ex.what() << "\n";
      return 1;
    }
    return 0;
  }

  if (out_target == OutTarget::Channel && rs485_path.empty()) {
    std::cerr << "err: --target channel needs --rs485 PATH "
                 "(or --port /dev/...)\n";
    return 1;
  }

  MidiInput midi;
  VoiceBank bank;
  AutoPlayer auto_player;
  PlayMode play_mode = auto_play ? PlayMode::Auto : PlayMode::Live;
  std::unique_ptr<AudioEngine> audio;
  std::unique_ptr<ChannelRs485Out> channel;

  try {
    midi.Open(midi_index);
    std::cout << "midi:   " << midi.PortName() << "\n";

    if (out_target == OutTarget::Speaker) {
      audio = std::make_unique<AudioEngine>();
      audio->Start(48000);
      std::cout << "output: speakers (" << audio->DeviceName() << " @ "
                << audio->SampleRate() << " Hz)\n";
    } else {
      channel = std::make_unique<ChannelRs485Out>();
      channel->Open(rs485_path, rs485_baud, gain_db, effect_echo);
      std::cout << "output: channel card via RS485 (" << channel->Path()
                << " @ " << rs485_baud << ", gain 1 " << channel->AttenDb()
                << ")\n";
      std::cout << "bus:    ASCII nX one-by-one (strict ACK)";
      if (effect_echo == host_io::rs485::EffectEcho::Leave) {
        std::cout << ", effect echo left unchanged";
      } else if (channel->EffectEchoDisabled()) {
        std::cout << ", effect echo off";
      } else if (effect_echo == host_io::rs485::EffectEcho::On) {
        std::cout << ", effect echo on";
      } else {
        std::cout << ", effect absent/no echo ack";
      }
      std::cout << "\n";
    }
    if (play_mode == PlayMode::Auto) {
      std::cout << "mode:   autoplay (key → live MIDI)\n";
      auto_player.Start(AutoPlayer::Clock::now());
    } else {
      std::cout << "mode:   live MIDI\n";
    }
    std::cout << "playing… (Enter to quit)\n";
  } catch (const std::exception& ex) {
    std::cerr << "err: " << ex.what() << "\n";
    return 1;
  }

  std::vector<NoteEvent> notes;
  bool running = true;
  while (running) {
    midi.Poll(notes);

    bool handoff_to_live = false;
    if (play_mode == PlayMode::Auto) {
      for (const NoteEvent& ne : notes) {
        if (ne.action == NoteAction::On || ne.action == NoteAction::AllOff) {
          handoff_to_live = true;
          break;
        }
      }
    }

    if (handoff_to_live) {
      auto_player.Stop();
      ApplyBankEvents(bank.AllOff(), audio.get(), channel.get(), bank);
      play_mode = PlayMode::Live;
      std::cout << "mode:   live MIDI\n";
      /* Fall through: process this poll's notes as live so the key sounds. */
    }

    if (play_mode == PlayMode::Auto) {
      /* Ignore Note Off while autoplaying — demo owns the bank. */
      ApplyBankEvents(auto_player.Tick(bank, AutoPlayer::Clock::now()),
                      audio.get(),
                      channel.get(),
                      bank);
    } else {
      for (const NoteEvent& ne : notes) {
        std::vector<BankEvent> events;
        if (ne.action == NoteAction::On) {
          events = bank.NoteOn(ne.key);
        } else if (ne.action == NoteAction::AllOff) {
          events = bank.AllOff();
        } else {
          events = bank.NoteOff(ne.key);
        }
        ApplyBankEvents(events, audio.get(), channel.get(), bank);
      }
    }

    if (channel && channel->BusFault()) {
      std::cerr << "err: RS485 note mode stopped (no ACK) — exiting\n";
      running = false;
      continue;
    }

    if (ConsumeQuitRequest()) {
      running = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  midi.Close();
  if (audio) {
    audio->Stop();
  }
  if (channel) {
    channel->Close();
  }
  return channel && channel->BusFault() ? 2 : 0;
}
