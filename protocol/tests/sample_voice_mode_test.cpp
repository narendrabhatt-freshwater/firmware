#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardlink/sample/client.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{

using cardlink::audio::SampleDryMixer;
using cardlink::audio::UrgentBurst;
using cardlink::midi::BankEvent;
using cardlink::midi::BankEventKind;
using cardlink::midi::VoiceBank;
using cardlink::sample::Client;
using cardlink::sample::NoteRequest;

constexpr uint8_t kC4 = 60u;
constexpr uint8_t kA4 = 69u;
constexpr uint8_t kC6 = 84u;

void Check(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void LoadTone(SampleDryMixer &mixer, uint16_t wave_id)
{
  std::vector<int16_t> body(8192u);
  for (size_t i = 0u; i < body.size(); ++i) {
    body[i] = static_cast<int16_t>((i * 37u) & 0x7FFFu);
  }
  std::string err;
  Check(mixer.SetBody(wave_id, body.data(), body.size(), err),
        "voice-mode body must load");
  mixer.SetAttackLen(wave_id, cardlink::audio::kAttackSamples);
  mixer.SetBodyRootHz(wave_id, cardlink::audio::kDefaultBodyRootHz);
}

unsigned ExpectedPrefill(double hz)
{
  return static_cast<unsigned>(std::ceil(
      hz / cardlink::audio::kDefaultBodyRootHz *
      static_cast<double>(cardlink::audio::kSampleRateHz) *
      static_cast<double>(cardlink::audio::kUrgentPrefillMs) / 1000.0));
}

struct PendingGate {
  NoteRequest note;
  Client::NoteGateDone done;
};

struct Harness {
  VoiceBank bank;
  Client client;
  std::deque<PendingGate> gates;

  explicit Harness(uint8_t voices)
  {
    Check(bank.SetVoiceLimit(voices).empty(), "voice limit must apply cleanly");
    LoadTone(client.Mixer(), kC4);
    LoadTone(client.Mixer(), kA4);
    LoadTone(client.Mixer(), kC6);
    client.SetNoteGate(
        [this](const NoteRequest &note, Client::NoteGateDone done) {
          gates.push_back(PendingGate{note, std::move(done)});
          return true;
        });
  }

  void Apply(const std::vector<BankEvent> &events)
  {
    std::array<NoteRequest, cardlink::audio::kSampleVoices> notes{};
    size_t count = 0u;
    for (const auto &event : events) {
      if (event.kind == BankEventKind::Off) {
        client.NoteOff(event.slot);
      } else if (event.kind == BankEventKind::On ||
                 event.kind == BankEventKind::Retrig) {
        notes[count++] = NoteRequest{event.slot, event.freq_hz,
                                     event.midi_key};
      }
      /* Steal only describes the retired key. Its following On is the one
       * authoritative nX restart for the reused physical slot. */
    }
    if (count != 0u) {
      Check(client.NoteOnBatch(notes.data(), count),
            "voice-mode note batch must queue");
    }
  }

  PendingGate AckNext()
  {
    Check(!gates.empty(), "expected an RS485 gate transaction");
    PendingGate gate = std::move(gates.front());
    gates.pop_front();
    gate.done(true);
    return gate;
  }

  UrgentBurst PopUrgent()
  {
    std::array<int16_t, cardlink::audio::kBodyBurstMax> body{};
    UrgentBurst burst{};
    Check(client.Mixer().FillUrgentBurst(body.data(), body.size(), burst),
          "ACKed note must expose an urgent BODY job");
    return burst;
  }

  std::vector<UrgentBurst> DrainUrgent()
  {
    std::vector<UrgentBurst> bursts;
    while (client.Mixer().UrgentPending()) {
      Check(bursts.size() < 256u, "urgent queue must make progress");
      bursts.push_back(PopUrgent());
    }
    return bursts;
  }
};

uint8_t CheckComplete(const std::vector<UrgentBurst> &bursts, uint8_t voice,
                      uint16_t wave_id, double hz, const char *message)
{
  unsigned total = 0u;
  unsigned sof_count = 0u;
  uint8_t session = 0xFFu;
  for (const auto &burst : bursts) {
    if (burst.voice != voice || burst.wave_id != wave_id) {
      continue;
    }
    if (session == 0xFFu) {
      session = burst.session;
    }
    Check(burst.session == session, message);
    total += burst.nsamp;
    sof_count += burst.sof ? 1u : 0u;
  }
  Check(session != 0xFFu && sof_count == 1u &&
            total >= ExpectedPrefill(hz) &&
            total <= cardlink::audio::kRingSamples,
        message);
  return session;
}

void TestMonoSteal()
{
  Harness h(1u);

  h.Apply(h.bank.NoteOn(kC6));
  Check(!h.client.Mixer().UrgentPending(),
        "mono C6 BODY must wait for nX ACK");
  const auto c6_gate = h.AckNext();
  Check(c6_gate.note.voice == 0u && c6_gate.note.wave_id == kC6,
        "mono C6 must use physical voice zero");
  const auto c6_bursts = h.DrainUrgent();
  const uint8_t c6_session = CheckComplete(
      c6_bursts, 0u, kC6, c6_gate.note.hz,
      "mono C6 chunks must build one complete urgent reservoir");

  const auto c4_events = h.bank.NoteOn(kC4);
  Check(c4_events.size() == 2u &&
            c4_events[0].kind == BankEventKind::Steal &&
            c4_events[0].midi_key == kC6 &&
            c4_events[1].slot == 0u,
        "mono C4 must steal C6 from voice zero");
  h.Apply(c4_events);
  const auto c4_gate = h.AckNext();
  const auto c4_bursts = h.DrainUrgent();
  const uint8_t c4_session = CheckComplete(
      c4_bursts, 0u, kC4, c4_gate.note.hz,
      "mono C4 replacement must get a complete chunked prefill");
  Check(!h.client.Mixer().BurstIsCurrent(0u, c6_session, kC6),
        "mono steal must stale the old C6 BODY session");
  Check(c4_session != c6_session,
        "mono C4 replacement must get a new session and prefill");

  const auto a4_events = h.bank.NoteOn(kA4);
  Check(a4_events.size() == 2u &&
            a4_events[0].midi_key == kC4 &&
            a4_events[1].slot == 0u,
        "mono A4 must steal C4 from voice zero");
  h.Apply(a4_events);
  const auto a4_gate = h.AckNext();
  const auto a4_bursts = h.DrainUrgent();
  const uint8_t a4_session = CheckComplete(
      a4_bursts, 0u, kA4, a4_gate.note.hz,
      "mono A4 must receive a complete chunked prefill");
  Check(a4_session != c4_session && h.bank.ActiveCount() == 1u &&
            h.bank.Slots()[0].midi_key == kA4,
        "mono sequence must finish with only A4 on voice zero");
}

void TestDuoStealAndChordPriority()
{
  Harness h(2u);
  const auto c6_events = h.bank.NoteOn(kC6);
  const auto c4_events = h.bank.NoteOn(kC4);
  h.Apply(c6_events);
  h.Apply(c4_events);
  Check(h.gates.size() == 2u && !h.client.Mixer().UrgentPending(),
        "duo chord BODY jobs must both wait behind their nX ACKs");

  const auto c6_gate = h.AckNext();
  const auto c4_gate = h.AckNext();
  uint8_t urgent_order[cardlink::audio::kSampleVoices]{};
  Check(h.client.Mixer().UrgentVoices(urgent_order) == 2u &&
            urgent_order[0] == 1u && urgent_order[1] == 0u,
        "shared urgent PACK must enumerate both pending chord voices");
  std::array<int16_t, cardlink::audio::kBodyBurstMax> body{};
  std::vector<UrgentBurst> chord_bursts;
  for (unsigned i = 0u; i < 2u; ++i) {
    UrgentBurst burst{};
    Check(h.client.Mixer().FillUrgentBurstForVoice(
              urgent_order[i], body.data(), body.size(),
              cardlink::audio::kUrgentQuantumMaxMs, burst) &&
              burst.sof,
          "first shared urgent quantum must carry each voice SOF");
    chord_bursts.push_back(burst);
  }
  auto remaining = h.DrainUrgent();
  chord_bursts.insert(chord_bursts.end(), remaining.begin(), remaining.end());
  const uint8_t c6_session = CheckComplete(
      chord_bursts, 0u, kC6, c6_gate.note.hz,
      "duo C6 chunks must complete without starvation");
  const uint8_t c4_session = CheckComplete(
      chord_bursts, 1u, kC4, c4_gate.note.hz,
      "duo C4 chunks must complete without starvation");
  const auto a4_events = h.bank.NoteOn(kA4);
  Check(a4_events.size() == 2u &&
            a4_events[0].kind == BankEventKind::Steal &&
            a4_events[0].midi_key == kC6 &&
            a4_events[0].slot == 0u &&
            a4_events[1].midi_key == kA4,
        "duo overflow must steal oldest C6, not newer C4");
  h.Apply(a4_events);
  const auto a4_gate = h.AckNext();
  Check(a4_gate.note.voice == 0u && a4_gate.note.wave_id == kA4,
        "duo A4 restart must target stolen voice zero");
  const auto a4_bursts = h.DrainUrgent();
  const uint8_t a4_session = CheckComplete(
      a4_bursts, 0u, kA4, a4_gate.note.hz,
      "duo A4 replacement chunks must complete");
  Check(a4_session != c6_session &&
            h.client.Mixer().BurstIsCurrent(1u, c4_session, kC4) &&
            h.bank.ActiveCount() == 2u &&
            h.bank.Slots()[0].midi_key == kA4 &&
            h.bank.Slots()[1].midi_key == kC4,
        "duo steal must finish as A4 + C4 while preserving C4 session");
}

void TestQueuedStealsDiscardSupersededBodies()
{
  {
    Harness mono(1u);
    mono.Apply(mono.bank.NoteOn(kC6));
    mono.Apply(mono.bank.NoteOn(kC4));
    mono.Apply(mono.bank.NoteOn(kA4));
    Check(mono.gates.size() == 3u && !mono.client.Mixer().UrgentPending(),
          "rapid mono gates must remain behind RS485 ACKs");
    (void)mono.AckNext();
    (void)mono.AckNext();
    const auto mono_a4_gate = mono.AckNext();
    const auto only = mono.DrainUrgent();
    CheckComplete(only, 0u, kA4, mono_a4_gate.note.hz,
                  "rapid mono must retain only A4 chunks");
    Check(!mono.client.Mixer().UrgentPending(),
          "rapid mono steals must discard queued C6/C4 BODY and keep A4");
  }

  {
    Harness duo(2u);
    duo.Apply(duo.bank.NoteOn(kC6));
    duo.Apply(duo.bank.NoteOn(kC4));
    duo.Apply(duo.bank.NoteOn(kA4));
    (void)duo.AckNext();
    const auto duo_c4_gate = duo.AckNext();
    const auto duo_a4_gate = duo.AckNext();
    const auto bursts = duo.DrainUrgent();
    CheckComplete(bursts, 0u, kA4, duo_a4_gate.note.hz,
                  "rapid duo A4 chunks must replace stolen C6");
    CheckComplete(bursts, 1u, kC4, duo_c4_gate.note.hz,
                  "rapid duo must retain C4 chunks");
    Check(!duo.client.Mixer().UrgentPending(),
          "rapid duo steal must queue A4 above C4 and discard stolen C6 BODY");
  }
}

} // namespace

int main()
{
  TestMonoSteal();
  TestDuoStealAndChordPriority();
  TestQueuedStealsDiscardSupersededBodies();
  return EXIT_SUCCESS;
}
