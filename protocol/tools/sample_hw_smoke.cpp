#include "cardlink/audio/sample_bulk.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardlink/midi/pitch.hpp"
#include "cardlink/rs485/controller.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/usb/stream_proto.hpp"
#include "cardproto/types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

bool WaitQueue(cardlink::rs485::Controller &bus,
               std::chrono::milliseconds timeout = 2s)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (bus.QueueDepth() == 0u) {
      std::this_thread::sleep_for(20ms);
      if (bus.QueueDepth() == 0u) {
        return true;
      }
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

} // namespace

int main(int argc, char **argv)
{
  const std::string which = argc > 1 ? argv[1] : "c6x3c5";
  const unsigned seconds = argc > 2
      ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10))
      : 12u;
  const std::string wave = argc > 3
      ? argv[3]
      : "cmi_control/waves/w48_sine_c4.raw";
  const std::string rs485 = argc > 4 ? argv[4] : "/dev/cu.usbserial-BG03CSYB";
  const std::string cdc = argc > 5
      ? argv[5]
      : "/dev/cu.usbmodemCHCARD_UAC_0093";

  std::array<double, cardlink::audio::kSampleVoices> hz{};
  unsigned nvoices = 0u;
  double root_hz = 260.0;
  bool mono_steal = false;
  bool duo_steal = false;
  if (which == "afg") {
    hz[0] = 880.0;       // A5
    hz[1] = 698.456463;  // F5
    hz[2] = 783.990872;  // G5
    nvoices = 3u;
  } else if (which == "c6") {
    hz[0] = 1046.502261;
    nvoices = 1u;
  } else if (which == "c6x2") {
    hz[0] = hz[1] = 1046.502261;
    nvoices = 2u;
  } else if (which == "c6x3") {
    hz[0] = hz[1] = hz[2] = 1046.502261;
    nvoices = 3u;
  } else if (which == "c6x3c5") {
    hz[0] = hz[1] = hz[2] = 1046.502261;
    nvoices = 3u;
    root_hz = 523.251131;
  } else if (which == "c6c4a4") {
    hz[0] = 1046.502261;
    hz[1] = 261.625565;
    hz[2] = 440.0;
    nvoices = 3u;
  } else if (which == "mono_steal") {
    hz[0] = 1046.502261; /* Worst phase is C6 alone. */
    nvoices = 1u;
    mono_steal = true;
  } else if (which == "duo_steal") {
    hz[0] = 1046.502261;
    hz[1] = 440.0; /* Worst final pair is C6/A4 or C4/A4. */
    nvoices = 2u;
    duo_steal = true;
  } else if (which == "c4x8") {
    hz.fill(261.625565);
    nvoices = 8u;
  } else if (which == "edge8") {
    hz.fill(295.8); /* Eight equal voices: about 437 source samples/ms. */
    nvoices = 8u;
  } else if (which == "edge8_420") {
    hz.fill(284.375); /* Eight equal voices: exactly 420 source samples/ms. */
    nvoices = 8u;
  } else if (which == "edge8_480") {
    hz.fill(325.0); /* Eight equal voices: exactly 480 source samples/ms. */
    nvoices = 8u;
  } else if (which == "edge8_500") {
    hz.fill(338.5416667); /* Eight voices: exactly 500 source samples/ms. */
    nvoices = 8u;
  } else {
    std::cerr << "case must be afg, c6, c6x2, c6x3, c6x3c5, c6c4a4, "
                 "mono_steal, duo_steal, c4x8, edge8, edge8_420, "
                 "edge8_480, or edge8_500\n";
    return EXIT_FAILURE;
  }

  cardlink::rs485::Controller bus;
  struct UsbStats {
    bool valid = false;
    uint32_t drop = 0u;
    uint32_t hold = 0u;
    uint32_t sof = 0u;
    uint32_t bad = 0u;
    uint32_t late = 0u;
  } usb_stats;
  std::mutex usb_stats_mutex;
  bus.SetLogHandler([](const std::string &s) { std::cout << s << '\n'; });
  bus.SetPollLogHandler([](const std::string &s) {
    if (s.rfind("warn:", 0) == 0) {
      std::cout << s << '\n';
    }
  });
  bus.SetCommandHandler(
      [&usb_stats, &usb_stats_mutex](cardproto::Target target,
                                     const std::string &command,
                                     const cardproto::Result &result) {
        if (target != cardproto::Target::Channel || command != "usb") {
          return;
        }
        UsbStats parsed;
        unsigned drop = 0u;
        unsigned hold = 0u;
        unsigned sof = 0u;
        unsigned bad = 0u;
        unsigned late = 0u;
        const std::string raw(result.raw);
        const auto read_counter = [&raw](const std::string &name,
                                          unsigned &value) {
          const auto at = raw.find(name);
          return at != std::string::npos &&
                 std::sscanf(raw.c_str() + at + name.size(), "%u", &value) ==
                     1;
        };
        if (read_counter("drop ", drop) && read_counter("hold ", hold) &&
            read_counter("sof ", sof) && read_counter("bad ", bad) &&
            read_counter("late ", late)) {
          parsed.valid = true;
          parsed.drop = drop;
          parsed.hold = hold;
          parsed.sof = sof;
          parsed.bad = bad;
          parsed.late = late;
        }
        std::lock_guard<std::mutex> lock(usb_stats_mutex);
        usb_stats = parsed;
      });
  if (!bus.Open(rs485, 921600u, 6u)) {
    std::cerr << "cannot open RS485 " << rs485 << '\n';
    return EXIT_FAILURE;
  }

  cardlink::sample::Client sample;
  sample.SetConsole([&bus](const std::string &cmd) {
    (void)bus.QueueExec(cardproto::Target::Channel, cmd);
  });
  sample.SetNoteGate(
      [&bus](const cardlink::sample::NoteRequest &note,
             cardlink::sample::Client::NoteGateStart start,
             cardlink::sample::Client::NoteGateDone done) {
        const auto queued = bus.QueueChannel(
            [note, start = std::move(start),
             done = std::move(done)](cardproto::ChannelClient &ch) {
              start();
              if (!note.note_on) {
                auto result = ch.NoteOff(note.voice);
                done(result.ok());
                return result;
              }
              const std::string aw =
                  "aw " + std::to_string(static_cast<unsigned>(note.voice)) +
                  " " + std::to_string(static_cast<unsigned>(note.wave_id));
              auto result = ch.Exec(aw);
              if (result.ok()) {
                result = ch.StreamNoteOn(note.voice, note.key, note.session);
              }
              done(result.ok());
              return result;
            });
        return queued == cardlink::rs485::QueueResult::Ok;
      });
  sample.SetCdcPath(cdc);
  bus.SetIdleHandler([&sample](uint8_t voice) { sample.Silence(voice); });

  std::string err;
  if (!sample.LoadWave(0u, wave, err) ||
      !sample.SetRootHz(0u, root_hz, err)) {
    std::cerr << "wave load failed: " << err << '\n';
    bus.Close();
    return EXIT_FAILURE;
  }
  if (!WaitQueue(bus)) {
    std::cerr << "RS485 queue stalled after wave load\n";
    bus.Close();
    return EXIT_FAILURE;
  }

  cardlink::audio::SampleBulkOut bulk;
  bulk.BindMixer(sample.Mixer());
  bus.SetVqHandler(
      [&bulk](uint8_t mask, uint8_t best,
              const std::array<uint16_t, cardlink::audio::kSampleVoices> &free,
              uint16_t last_pack_sequence) {
        bulk.SubmitStatus(mask, best, free, last_pack_sequence);
      });
  if (!bulk.Start(err)) {
    std::cerr << "BODY open failed: " << err << '\n';
    bus.Close();
    return EXIT_FAILURE;
  }
  /* The earlier UAC experiment could open once but failed on its second
   * CoreAudio SET_INTERFACE cycle. Prove restart before measuring notes. */
  std::this_thread::sleep_for(100ms);
  bulk.Stop();
  std::this_thread::sleep_for(100ms);
  if (!bulk.Start(err)) {
    std::cerr << "16-bit UAC restart failed: " << err << '\n';
    bus.Close();
    return EXIT_FAILURE;
  }
  std::cout << "ok: 10ch 51k signed-int16 UAC opened twice\n";

  double aggregate_samples_per_ms = 0.0;
  for (uint8_t v = 0u; v < nvoices; ++v) {
    aggregate_samples_per_ms += 48.0 * hz[v] / root_hz;
  }
  const double capacity_samples_per_ms =
      static_cast<double>(cardlink::usb::kStreamBodySamplesPerMs);
  std::cout << "budget: demand=" << aggregate_samples_per_ms
            << " samples/ms capacity=" << capacity_samples_per_ms << '\n';
  if (aggregate_samples_per_ms > capacity_samples_per_ms) {
    std::cout << "warn: case is mathematically over the 16-bit UAC budget; "
                 "hold is expected\n";
  }

  cardlink::midi::VoiceBank smoke_bank;
  if (mono_steal || duo_steal) {
    (void)smoke_bank.SetVoiceLimit(mono_steal ? 1u : 2u);
  }
  const auto apply_events = [&sample](
                                const std::vector<cardlink::midi::BankEvent>
                                    &events) {
    std::array<cardlink::sample::NoteRequest,
               cardlink::audio::kSampleVoices> pending{};
    size_t count = 0u;
    for (const auto &event : events) {
      if (event.kind == cardlink::midi::BankEventKind::Off) {
        sample.NoteOff(event.slot);
      } else if (event.kind == cardlink::midi::BankEventKind::On ||
                 event.kind == cardlink::midi::BankEventKind::Retrig) {
        /* This smoke tool loads one wave at id 0; pitch comes from the raw key. */
        pending[count++] =
            cardlink::sample::NoteRequest{event.slot, event.midi_key, 0u, 0xFFu, true};
      }
    }
    return count == 0u || sample.NoteOnBatch(pending.data(), count);
  };

  /* Each invocation must own its startup counters; a prior failed smoke run
   * may have exited before reaching the normal measurement reset. */
  (void)bus.QueueExec(cardproto::Target::Channel, "usb 0");
  (void)WaitQueue(bus);

  if (mono_steal || duo_steal) {
    if (!apply_events(smoke_bank.NoteOn(84u))) { /* C6 */
      std::cerr << "cannot start C6 steal sequence\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
  } else {
    std::array<cardlink::sample::NoteRequest,
               cardlink::audio::kSampleVoices> notes{};
    for (uint8_t v = 0u; v < nvoices; ++v) {
      notes[v] = cardlink::sample::NoteRequest{
          v, cardlink::midi::HzToNearestMidi(hz[v]), 0u, 0xFFu, true};
    }
    if (!sample.NoteOnBatch(notes.data(), nvoices)) {
      std::cerr << "cannot start chord: no loaded BODY\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
  }
  if (!WaitQueue(bus)) {
    std::cerr << "RS485 queue stalled at note-on\n";
  }
  /* QueueDepth excludes Controller's coalesced nX state. Let every audible
   * note command complete before defining the measured steady-state window. */
  std::this_thread::sleep_for(1s);
  (void)bus.QueueExec(cardproto::Target::Channel, "usb");
  (void)WaitQueue(bus);
  UsbStats startup_stats;
  {
    std::lock_guard<std::mutex> lock(usb_stats_mutex);
    startup_stats = usb_stats;
  }
  const uint32_t expected_start_sof = (mono_steal || duo_steal) ? 1u : nvoices;
  const auto startup_schedule = bulk.LastUrgentSchedule();
  std::cout << "urgent: voices=" << startup_schedule.voices
            << " demand=" << startup_schedule.demand_samples_per_ms
            << " samples/ms attack_left="
            << startup_schedule.remaining_attack_ms
            << " ms quantum=" << startup_schedule.quantum_ms << " ms\n";
  const bool startup_ok =
      startup_stats.valid && startup_stats.drop == 0u &&
      startup_stats.hold == 0u && startup_stats.bad == 0u &&
      startup_stats.late == 0u && startup_stats.sof == expected_start_sof;
  if (!startup_ok) {
    std::cerr << "FAIL startup: drop=" << startup_stats.drop
              << " hold=" << startup_stats.hold
              << " sof=" << startup_stats.sof
              << " expected_sof=" << expected_start_sof
              << " bad=" << startup_stats.bad
              << " late=" << startup_stats.late << '\n';
  }
  (void)bus.QueueExec(cardproto::Target::Channel, "usb 0");
  (void)WaitQueue(bus);
  std::cout << "RUN " << which << " voices=" << nvoices
            << " seconds=" << seconds << '\n';
  if (mono_steal || duo_steal) {
    const auto phase = std::chrono::milliseconds(
        std::max(1u, seconds * 1000u / 3u));
    std::this_thread::sleep_for(phase);
    std::cout << "STEAL/ADD C4\n";
    const auto c4_events = smoke_bank.NoteOn(60u);
    if ((mono_steal &&
         (c4_events.size() != 2u || c4_events[0].midi_key != 84u ||
          c4_events[1].slot != 0u)) ||
        (duo_steal &&
         (c4_events.size() != 1u || c4_events[0].slot != 1u))) {
      std::cerr << "voice-bank C4 allocation mismatch\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
    if (!apply_events(c4_events)) {
      std::cerr << "cannot apply C4 allocation\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
    (void)WaitQueue(bus);
    std::this_thread::sleep_for(phase);
    std::cout << "STEAL A4\n";
    const auto a4_events = smoke_bank.NoteOn(69u);
    const uint8_t expected_stolen = mono_steal ? 60u : 84u;
    if (a4_events.size() != 2u ||
        a4_events[0].kind != cardlink::midi::BankEventKind::Steal ||
        a4_events[0].midi_key != expected_stolen ||
        a4_events[1].slot != 0u) {
      std::cerr << "voice-bank A4 steal mismatch\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
    if (!apply_events(a4_events)) {
      std::cerr << "cannot apply A4 steal\n";
      bulk.Stop();
      bus.Close();
      return EXIT_FAILURE;
    }
    (void)WaitQueue(bus);
    std::this_thread::sleep_for(phase);
  } else {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
  }

  (void)bus.QueueExec(cardproto::Target::Channel, "vq");
  (void)WaitQueue(bus);

  sample.AllNotesOff();
  for (uint8_t v = 0u; v < nvoices; ++v) {
    sample.Silence(v);
  }
  (void)WaitQueue(bus);
  std::this_thread::sleep_for(100ms);
  const uint32_t xruns = bulk.XrunCount();
  const uint64_t render_frames = bulk.RenderFrameCount();
  bulk.Stop();
  std::cout << "host: CoreAudio render_frames=" << render_frames
            << " callback_xruns=" << xruns << '\n';
  (void)bus.QueueExec(cardproto::Target::Channel, "usb");
  (void)WaitQueue(bus);
  UsbStats final_stats;
  {
    std::lock_guard<std::mutex> lock(usb_stats_mutex);
    final_stats = usb_stats;
  }
  bus.Close();
  if (!final_stats.valid) {
    std::cerr << "cannot parse final USB statistics\n";
    return EXIT_FAILURE;
  }
  if (!startup_ok || xruns != 0u || final_stats.drop != 0u ||
      final_stats.hold != 0u ||
      final_stats.bad != 0u || final_stats.late != 0u ||
      final_stats.sof != ((mono_steal || duo_steal) ? 2u : 0u)) {
    std::cerr << "FAIL: xruns=" << xruns << " drop=" << final_stats.drop
              << " hold=" << final_stats.hold << " bad=" << final_stats.bad
              << " late=" << final_stats.late
              << " sof=" << final_stats.sof << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "PASS: zero host xruns, drops, holds, bad frames, and late fills\n";
  return EXIT_SUCCESS;
}
