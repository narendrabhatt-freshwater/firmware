#include "cardlink/audio/sample_dry.hpp"

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

unsigned FillUntilStop(cardlink::audio::SampleDryMixer &mixer, uint8_t voice)
{
  std::array<int16_t, cardlink::audio::kBodyBurstMax> buf{};
  unsigned total = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const unsigned want = mixer.WantBurst(voice);
    if (want == 0) {
      break;
    }
    bool sof = false;
    uint8_t session = 0;
    const unsigned got = mixer.FillBurst(voice, buf.data(), want, sof, session);
    Check(got == want, "FillBurst must match WantBurst");
    if (total == 0) {
      Check(sof, "first burst of a session must set SOF");
    } else {
      Check(!sof, "later bursts must not set SOF");
    }
    total += got;
  }
  return total;
}

unsigned StopFill1x()
{
  return (cardlink::audio::kStopMs * cardlink::audio::kSampleRateHz) / 1000u;
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
        "C4 note-on must want a full burst with no poll");
  const unsigned filled = FillUntilStop(mixer, 0);
  Check(filled == StopFill1x(),
        "C4 note-on must stop at ~25 ms of buffer, not a full ring");
  Check(mixer.WantBurst(0) == 0, "enough play time must not oversend");

  std::array<uint8_t, kSampleVoices> stale{};
  stale.fill(kVqSlotEmpty);
  stale[0] = 14; /* 3584 free — FIFO lag; card has not accepted BODY yet */
  mixer.ApplyVoiceQuery(0x01, 0, stale.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0,
        "stale low vq must not reopen a voice that already has enough time");

  mixer.ConsumeOutputSamples(479.0);
  Check(mixer.WantBurst(0) == 0,
        "attack must not drain fill (would overflow the ring)");

  mixer.ConsumeOutputSamples(1.0 + static_cast<double>(kBodyBurstMax));
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "C4 consume after the join must reopen a burst");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  {
    std::array<int16_t, kBodyBurstMax> buf{};
    bool sof = false;
    uint8_t session = 0;
    Check(mixer.FillBurst(0, buf.data(), kBodyBurstMax, sof, session) ==
              kBodyBurstMax,
          "one burst after note-on");
  }
  std::array<uint8_t, kSampleVoices> full_vq{};
  full_vq.fill(0);
  mixer.ApplyVoiceQuery(0x01, 0, full_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0, "vq full must stop even with only 512 queued");
  std::array<uint8_t, kSampleVoices> empty_vq{};
  empty_vq.fill(kVqSlotEmpty);
  mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "a later empty vq must not leave fill stuck at a full-ring snap");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  {
    std::array<int16_t, kBodyBurstMax> buf{};
    bool sof = false;
    uint8_t session = 0;
    Check(mixer.FillBurst(0, buf.data(), kBodyBurstMax, sof, session) ==
              kBodyBurstMax,
          "one burst to leave host fill below 25 ms");
  }
  std::array<uint8_t, kSampleVoices> nearly_full{};
  nearly_full.fill(kVqSlotEmpty);
  nearly_full[0] = 1; /* 256 free → ~3840 in the ring, far more than 25 ms */
  mixer.ApplyVoiceQuery(0x01, 0, nearly_full.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0,
        "vq with 25 ms already on the card must not send a 512 into 256 free");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 0.5); /* half speed vs C4 root */
  mixer.ConsumeOutputSamples(0.0);
  const unsigned c3 = FillUntilStop(mixer, 0);
  Check(c3 == StopFill1x() / 2u,
        "half-speed notes must keep half the samples for the same time");
  std::array<uint8_t, kSampleVoices> slots{};
  slots.fill(kVqSlotEmpty);
  slots[0] = 2; /* 512 free → fill 3584, far more than 25 ms @ 0.5× */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0,
        "vq must stop a low note that already has enough time");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() < kSampleVoices, "two empty notes both need data");
  (void)FillUntilStop(mixer, 0);
  Check(mixer.HungriestWant() == 1,
        "after filling voice 0, voice 1 is hungriest");
  slots.fill(kVqSlotEmpty);
  mixer.ApplyVoiceQuery(0x03, 1, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() == 1, "card best must win when it wants data");

  mixer.Silence(0);
  mixer.Silence(1);
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 8.0);
  mixer.ConsumeOutputSamples(0.0);
  const unsigned fast = FillUntilStop(mixer, 0);
  Check(fast == cardlink::audio::kRingSamples - cardlink::audio::kRingHeadroom,
        "fast notes must cap at ring headroom");
  slots.fill(kVqSlotEmpty);
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0,
        "stale empty vq must not refill a ring already at headroom");

  return EXIT_SUCCESS;
}
