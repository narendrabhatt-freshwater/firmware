#include "cmi/core.hpp"
#include "cardlink/midi/midi_input.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardlink/vm/compiler.hpp"
#include "cardproto/channel.hpp"

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
  using cardlink::midi::BankEventKind;
  using cardlink::midi::DecodeNoteMessage;
  using cardlink::midi::NoteAction;

  const auto low = DecodeNoteMessage({0x90u, 60u, 1u});
  const auto middle = DecodeNoteMessage({0x90u, 61u, 64u});
  const auto high = DecodeNoteMessage({0x90u, 62u, 127u});
  const auto zero = DecodeNoteMessage({0x90u, 63u, 0u});
  Expect(low && low->action == NoteAction::On && low->velocity == 1u,
         "minimum MIDI velocity must be preserved");
  Expect(middle && middle->velocity == 64u,
         "middle MIDI velocity must be preserved");
  Expect(high && high->velocity == 127u,
         "maximum MIDI velocity must be preserved");
  Expect(zero && zero->action == NoteAction::Off && zero->velocity == 0u,
         "velocity-zero Note On must decode as Note Off");

  cardlink::midi::VoiceBank voices;
  auto events = voices.NoteOn(60u, 1u);
  Expect(events.size() == 1u && events[0].velocity == 1u,
         "voice allocation must preserve velocity");
  events = voices.NoteOn(60u, 64u);
  Expect(events.size() == 1u && events[0].kind == BankEventKind::Retrig &&
             events[0].velocity == 64u,
         "voice retrigger must replace velocity");
  for (uint8_t key = 61u; key < 68u; ++key) {
    (void)voices.NoteOn(key, static_cast<uint8_t>(key));
  }
  events = voices.NoteOn(68u, 127u);
  Expect(events.size() == 2u && events[0].kind == BankEventKind::Steal &&
             events[1].kind == BankEventKind::On &&
             events[1].velocity == 127u,
         "voice stealing must preserve replacement velocity");
  Expect(voices.NoteOn(69u, 0u).empty(),
         "voice allocation must reject zero velocity");

  Expect(cardproto::FormatNoteOn(0u, 69u, 100u) == "n0 on 69 100",
         "note formatter must include velocity");
  Expect(cardproto::FormatStreamNoteOn(7u, 1u, 127u, 254u) ==
             "n7 on 1 127 @254",
         "stream note formatter must include velocity and session");
  Expect(cardproto::FormatStreamNoteOn(7u, 1u, 254u) ==
             "n7 on 1 127 @254",
         "legacy stream formatter must default to full velocity");

  const std::string handlers =
      "def on_note_off()\nend\n"
      "def on_ramp_end()\nend\n";
  cardlink::vm::BerryCompiler compiler;
  const auto abi1 = compiler.CompileChannel(
      "def on_note_on(key, velocity)\n    start_note()\nend\n" + handlers);
  Expect(abi1.ok && abi1.program.size() >= 10u && abi1.program[8] == 1u &&
             abi1.program[9] == 0u,
         "zero-argument note-off handler must compile as ABI1");
  const auto legacy_note_on = compiler.CompileChannel(
      "def on_note_on(key)\n    start_note()\nend\n" + handlers);
  Expect(!legacy_note_on.ok,
         "one-argument note-on handler must be rejected by ABI1 compiler");
  const auto pending_note_off = compiler.CompileChannel(
      "def on_note_on(key, velocity)\n    start_note()\nend\n"
      "def on_note_off(has_pending)\nend\n"
      "def on_ramp_end()\nend\n");
  Expect(!pending_note_off.ok,
         "legacy pending argument must be rejected by ABI1 compiler");

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
