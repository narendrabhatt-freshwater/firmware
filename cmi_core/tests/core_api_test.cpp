#include "cmi/core.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

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
  cmi::Core core({});
  Expect(!core.isConnected(), "a new Core must be disconnected");
  Expect(!core.isReady(), "a new Core must not be ready");
  Expect(core.loadedProgramMask() == 0u,
         "a new Core must not report loaded voice programs");

  const cmi::Result invalid_voice =
      core.loadVoiceScript(8u, "missing.be");
  Expect(invalid_voice.code == cmi::ErrorCode::InvalidArgument,
         "program loading must reject an invalid voice");

  const cmi::Result connect = core.connect();
  Expect(connect.code == cmi::ErrorCode::InvalidArgument,
         "connect must reject missing hardware ports");

  cmi::SampleDefinition invalid_sample;
  invalid_sample.sample_file = "missing.wav";
  invalid_sample.attack_file = "attack.i16";
  invalid_sample.body_file = "body.i16";
  const cmi::Result sample = core.loadSample(invalid_sample);
  Expect(sample.code == cmi::ErrorCode::InvalidArgument,
         "loadSample must reject mixed combined and split inputs");

  std::array<uint16_t, 128> map{};
  map.fill(256u);
  const cmi::Result midi_map = core.setMidiSampleMap(map);
  Expect(midi_map.code == cmi::ErrorCode::InvalidArgument,
         "MIDI mappings must reject invalid sample IDs");

  const cmi::Result note = core.sampleNoteOn(0u, 60u, 0u);
  Expect(note.code == cmi::ErrorCode::NotConnected,
         "sampleNoteOn must reject disconnected use");

  Expect(core.disconnect().ok(), "disconnect must be idempotent");
  return 0;
}
