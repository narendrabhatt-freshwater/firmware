#include "cardlink/audio/sample_bulk.hpp"
#include "cardlink/rs485/controller.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/usb/stream_proto.hpp"
#include "cardproto/types.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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
      : "/dev/cu.usbmodemCHCARD_0012";

  std::array<double, cardlink::audio::kSampleVoices> hz{};
  unsigned nvoices = 0u;
  double root_hz = 260.0;
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
  } else if (which == "c4x8") {
    hz.fill(261.625565);
    nvoices = 8u;
  } else if (which == "edge8") {
    hz.fill(295.8); /* Eight equal voices: about 437 source samples/ms. */
    nvoices = 8u;
  } else if (which == "edge8_420") {
    hz.fill(284.375); /* Eight equal voices: exactly 420 source samples/ms. */
    nvoices = 8u;
  } else {
    std::cerr << "case must be afg, c6, c6x2, c6x3, c6x3c5, c4x8, edge8, "
                 "or edge8_420\n";
    return EXIT_FAILURE;
  }

  cardlink::rs485::Controller bus;
  bus.SetLogHandler([](const std::string &s) { std::cout << s << '\n'; });
  bus.SetPollLogHandler([](const std::string &s) {
    if (s.rfind("warn:", 0) == 0) {
      std::cout << s << '\n';
    }
  });
  if (!bus.Open(rs485, 921600u, 6u)) {
    std::cerr << "cannot open RS485 " << rs485 << '\n';
    return EXIT_FAILURE;
  }

  cardlink::sample::Client sample;
  sample.SetConsole([&bus](const std::string &cmd) {
    (void)bus.QueueExec(cardproto::Target::Channel, cmd);
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
  std::cout << "ok: 10ch 48k signed-int16 UAC opened twice\n";

  double aggregate_samples_per_ms = 0.0;
  for (uint8_t v = 0u; v < nvoices; ++v) {
    aggregate_samples_per_ms += 48.0 * hz[v] / root_hz;
  }
  const double capacity_samples_per_ms =
      static_cast<double>(cardlink::usb::PackMaxSamples(nvoices)) / 10.0;
  std::cout << "budget: demand=" << aggregate_samples_per_ms
            << " samples/ms capacity=" << capacity_samples_per_ms << '\n';
  if (aggregate_samples_per_ms > capacity_samples_per_ms) {
    std::cout << "warn: case is mathematically over the 16-bit UAC budget; "
                 "hold is expected\n";
  }

  std::array<cardlink::sample::NoteRequest,
             cardlink::audio::kSampleVoices> notes{};
  for (uint8_t v = 0u; v < nvoices; ++v) {
    notes[v] = cardlink::sample::NoteRequest{v, hz[v], 0u};
  }
  if (!sample.NoteOnBatch(notes.data(), nvoices)) {
    std::cerr << "cannot start chord: no loaded BODY\n";
    bulk.Stop();
    bus.Close();
    return EXIT_FAILURE;
  }
  if (!WaitQueue(bus)) {
    std::cerr << "RS485 queue stalled at note-on\n";
  }
  /* QueueDepth excludes Controller's coalesced nX state. Let every audible
   * note command complete before defining the measured steady-state window. */
  std::this_thread::sleep_for(1s);
  (void)bus.QueueExec(cardproto::Target::Channel, "usb 0");
  (void)WaitQueue(bus);
  std::cout << "RUN " << which << " voices=" << nvoices
            << " seconds=" << seconds << '\n';
  std::this_thread::sleep_for(std::chrono::seconds(seconds));

  (void)bus.QueueExec(cardproto::Target::Channel, "vq");
  (void)WaitQueue(bus);

  sample.AllNotesOff();
  for (uint8_t v = 0u; v < nvoices; ++v) {
    sample.Silence(v);
  }
  (void)WaitQueue(bus);
  std::this_thread::sleep_for(100ms);
  const uint32_t xruns = bulk.XrunCount();
  bulk.Stop();
  std::cout << "host: CoreAudio callback xruns=" << xruns << '\n';
  (void)bus.QueueExec(cardproto::Target::Channel, "usb");
  (void)WaitQueue(bus);
  bus.Close();
  return EXIT_SUCCESS;
}
