#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/usb/stream_proto.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{

void Check(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void LoadTone(cardlink::audio::SampleDryMixer &mixer, uint16_t wave_id)
{
  std::vector<int16_t> body(8192);
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] = static_cast<int16_t>(i & 0x7fff);
  }
  std::string err;
  Check(mixer.SetBody(wave_id, body.data(), body.size(), err),
        "SetBody must accept a dummy loop");
  mixer.SetAttackLen(wave_id, cardlink::audio::kAttackSamples);
  mixer.SetBodyRootHz(wave_id, cardlink::audio::kDefaultBodyRootHz);
}

unsigned TakeBurst(cardlink::audio::SampleDryMixer &mixer, uint8_t voice)
{
  std::array<int16_t, cardlink::audio::kBodyBurstMax> buf{};
  bool sof = false;
  uint8_t session = 0;
  const unsigned want = mixer.WantBurst(voice);
  if (want == 0) {
    return 0;
  }
  const unsigned got = mixer.FillBurst(voice, buf.data(), want, sof, session);
  Check(got == want, "FillBurst must match WantBurst");
  return got;
}

} // namespace

int main()
{
  using cardlink::audio::SampleDryMixer;
  using cardlink::audio::kBodyBurstMax;
  using cardlink::audio::kDefaultBodyRootHz;
  using cardlink::audio::kSampleVoices;
  using cardlink::audio::kVqSlotEmpty;

  SampleDryMixer mixer;
  LoadTone(mixer, 0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "C4 note-on must want one SOF burst with no poll");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "first burst must send 512");
  Check(mixer.WantBurst(0) == 0, "after 512, wait for vq");

  std::array<uint8_t, kSampleVoices> empty_vq{};
  empty_vq.fill(kVqSlotEmpty);
  std::array<uint8_t, kSampleVoices> nearly_empty{};
  nearly_empty.fill(kVqSlotEmpty);
  nearly_empty[0] = 14; /* filled 1..512 — playhead can already be at 4 */
  mixer.ApplyVoiceQuery(0x01, 0, nearly_empty.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "slot 14 must refill; 4 samples is a hold, not 5 ms of buffer");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "slot 14 gets one burst");
  mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  for (unsigned i = 0; i < 5u; ++i) {
    Check(mixer.WantBurst(0) == kBodyBurstMax,
          "fresh vq may spend only predicted remaining ring credit");
    Check(TakeBurst(mixer, 0) == kBodyBurstMax,
          "stale snapshot credit must remain bounded by predicted fill");
    mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
    mixer.ConsumeOutputSamples(0.0);
  }
  Check(mixer.WantBurst(0) == 0,
        "stale vq must retain one maximum in-flight burst of headroom");
  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "SOF after slot-14 test");

  mixer.ConsumeOutputSamples(2000.0);
  Check(mixer.WantBurst(0) == 0, "wall-clock must not send without a new vq");

  mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "empty vq must ask for one burst");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "one burst per vq");
  Check(mixer.WantBurst(0) == 0, "already served this vq");

  std::array<uint8_t, kSampleVoices> slots{};
  slots.fill(kVqSlotEmpty);
  slots[0] = 13; /* min fill 513 — last bin before the hold zone */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "slot 13 must refill; slot 14 is already 1..512");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "slot 13 gets one burst");

  slots.fill(kVqSlotEmpty);
  slots[0] = 12; /* 3072 samples of safe free-space permission */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "any fresh vq free-space grant must top up the jitter ring");

  std::array<uint8_t, kSampleVoices> full_vq{};
  full_vq.fill(0);
  mixer.ApplyVoiceQuery(0x01, 0, full_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0, "vq free-slot 0 is a hard stop");

  mixer.ConsumeOutputSamples(2000.0);
  mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "a later empty vq must send again");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 0.5);
  mixer.ConsumeOutputSamples(0.0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "C3 still gets one SOF burst");
  slots.fill(kVqSlotEmpty);
  slots[0] = 2; /* 512 samples of safe free-space permission */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "low note must use fresh vq credit instead of a time heuristic");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() < kSampleVoices, "two empty notes both need data");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "voice 0 SOF");
  Check(mixer.HungriestWant() == 1, "after voice 0 prefill, voice 1 is hungriest");
  slots.fill(kVqSlotEmpty);
  mixer.ApplyVoiceQuery(0x03, 1, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() == 1, "card best must win when it wants data");

  mixer.Silence(0);
  mixer.Silence(1);
  mixer.ConsumeOutputSamples(0.0);

  Check(cardlink::usb::PackMaxSamples(1) == kBodyBurstMax,
        "one-voice bulk pack remains bounded by BODY nsamp max");
  Check(cardlink::usb::PackMaxSamples(5) >= 960,
        "five C5 voices must fit in one FS bulk frame");
  Check(cardlink::usb::PackMaxSamples(8) >= 1152,
        "multi-voice permission must use the two-frame transfer budget");

  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(2, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(3, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(4, 0, kDefaultBodyRootHz * 2.0);
  mixer.ConsumeOutputSamples(0.0);
  {
    uint8_t order[kSampleVoices];
    const unsigned nwant = mixer.WantingVoices(order);
    Check(nwant == 5, "five C5 notes must all want the SOF burst");
    Check(cardlink::usb::PackMaxSamples(nwant) >= 480,
          "packed bulk payload must cover 5xC5 consume");
    unsigned grants[kSampleVoices]{};
    const unsigned budget = cardlink::usb::PackMaxSamples(nwant);
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "fair allocator must use the available PACK payload");
    for (unsigned i = 0; i < nwant; ++i) {
      Check(grants[i] > 0u,
            "fair allocator must not starve a wanting voice");
    }
  }

  mixer.AllNotesOff();
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(2, 0, kDefaultBodyRootHz * 4.0);
  mixer.ConsumeOutputSamples(0.0);
  {
    uint8_t order[kSampleVoices];
    const unsigned nwant = mixer.WantingVoices(order);
    unsigned grants[kSampleVoices]{};
    unsigned by_voice[kSampleVoices]{};
    const unsigned budget = cardlink::usb::PackMaxSamples(nwant);
    Check(nwant == 3, "C4+C5+C6 must all want initial BODY");
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "C4+C5+C6 must use the full permitted PACK");
    for (unsigned i = 0; i < nwant; ++i) {
      by_voice[order[i]] = grants[i];
    }
    Check(by_voice[0] >= 227u && by_voice[0] <= 231u &&
              by_voice[1] >= 457u && by_voice[1] <= 461u &&
              by_voice[2] == kBodyBurstMax,
          "C4+C5+C6 grants must be weighted, then redistribute C6's cap");
  }

  return EXIT_SUCCESS;
}
