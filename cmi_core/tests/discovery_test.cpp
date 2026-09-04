#include "core/detail.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Expect(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main()
{
  cmi::DiscoveryOptions options;
  const std::vector<cmi::MidiPort> midi{{0u, "Keyboard"}};
  auto report = cmi::detail::BuildSetupReport(
      {}, options,
      {"/dev/ttyUSB0", "/dev/ttyACM0"},
      {"Channel Card BODY"}, midi);
  Expect(report.ready(), "one complete device set must be ready");
  Expect(report.params.rs485_port == "/dev/ttyUSB0",
         "unique RS485 adapter must be selected");
  Expect(report.params.channel_cdc_port == "/dev/ttyACM0",
         "unique Channel CDC must be selected");
  Expect(report.params.channel_audio_device == "Channel Card BODY",
         "unique Channel audio device must be selected");
  Expect(report.params.midi_port == "Keyboard",
         "unique MIDI device must be selected");

  report = cmi::detail::BuildSetupReport(
      {}, options,
      {"/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyACM0"},
      {"Channel Card BODY"}, {});
  Expect(!report.ready(), "ambiguous RS485 adapters must not be guessed");
  Expect(report.params.rs485_port.empty(),
         "ambiguous RS485 selection must remain empty");

  cmi::CoreParams explicit_params;
  explicit_params.rs485_port = "/chosen/adapter";
  explicit_params.channel_cdc_port = "/chosen/channel";
  explicit_params.channel_audio_device = "Chosen Audio";
  explicit_params.midi_port = "Chosen MIDI";
  report = cmi::detail::BuildSetupReport(
      explicit_params, options, {}, {}, {});
  Expect(report.ready(), "explicit overrides must work without discovery");
  Expect(report.params.rs485_port == "/chosen/adapter" &&
             report.params.midi_port == "Chosen MIDI",
         "discovery must preserve explicit overrides");

  options.require_audio = false;
  options.select_midi = false;
  report = cmi::detail::BuildSetupReport(
      {}, options, {"/dev/ttyUSB0", "/dev/ttyACM0"}, {}, {});
  Expect(report.ready(), "optional audio and MIDI may be absent");
  return 0;
}
