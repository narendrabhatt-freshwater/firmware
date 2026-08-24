#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/usb/stream_proto.hpp"

#include <array>
#include <cmath>
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

void GrantEmpty(cardlink::audio::SampleDryMixer &mixer, uint8_t mask,
                uint8_t best = 0xFFu)
{
  std::array<uint8_t, cardlink::audio::kSampleVoices> slots{};
  slots.fill(cardlink::audio::kVqSlotEmpty);
  mixer.ApplyVoiceQuery(mask, best, slots.data());
  mixer.ConsumeOutputSamples(0.0);
}

void RunNoFaultCase(const std::array<double, cardlink::audio::kSampleVoices> &inc,
                    unsigned nvoices, unsigned quanta, const char *message,
                    double body_root_hz = cardlink::audio::kDefaultBodyRootHz)
{
  using namespace cardlink::audio;
  SampleDryMixer mixer;
  LoadTone(mixer, 0);
  mixer.SetBodyRootHz(0, body_root_hz);
  std::array<unsigned, kSampleVoices> ring{};
  std::array<double, kSampleVoices> phase{};
  uint8_t mask = 0u;
  const double output_frames = 48.0 *
      static_cast<double>(cardlink::usb::kStreamServiceFrames);

  for (uint8_t v = 0; v < nvoices; ++v) {
    mixer.NoteOn(v, 0, body_root_hz * inc[v]);
    mixer.ConsumeOutputSamples(0.0);
    ring[v] = TakeBurst(mixer, v);
    Check(ring[v] == kBodyBurstMax,
          "capacity simulation requires a complete SOF prefill");
    mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(1u << v));
  }

  for (unsigned q = 0; q < quanta; ++q) {
    std::array<uint16_t, kSampleVoices> free_samples{};
    free_samples.fill(static_cast<uint16_t>(kRingSamples));
    uint8_t best = 0xFFu;
    double best_ms = 0.0;
    for (uint8_t v = 0; v < nvoices; ++v) {
      phase[v] += inc[v] * output_frames;
      const unsigned consume = static_cast<unsigned>(std::floor(phase[v]));
      phase[v] -= static_cast<double>(consume);
      Check(ring[v] >= consume, message);
      ring[v] -= consume;
      free_samples[v] = static_cast<uint16_t>(kRingSamples - ring[v]);
      const double ms = static_cast<double>(ring[v]) / inc[v];
      if (best == 0xFFu || ms < best_ms) {
        best = v;
        best_ms = ms;
      }
    }

    mixer.ApplyVoiceStatus(mask, best, free_samples.data());
    mixer.ConsumeOutputSamples(output_frames);
    uint8_t order[kSampleVoices]{};
    const unsigned nwant = mixer.WantingVoices(order);
    if (nwant == 0u) {
      continue;
    }
    unsigned grants[kSampleVoices]{};
    const unsigned budget = cardlink::usb::PackMaxSamples(nwant);
    (void)mixer.AllocateBursts(order, nwant, budget, grants);
    for (unsigned i = 0; i < nwant; ++i) {
      const uint8_t v = order[i];
      std::array<int16_t, kBodyBurstMax> body{};
      bool sof = false;
      uint8_t session = 0;
      const unsigned got = mixer.FillBurst(v, body.data(), grants[i], sof,
                                           session);
      Check(ring[v] + got <= kRingSamples, message);
      ring[v] += got;
    }
  }
}

} // namespace

int main()
{
  using cardlink::audio::SampleDryMixer;
  using cardlink::audio::kBodyBurstMax;
  using cardlink::audio::kDefaultBodyRootHz;
  using cardlink::audio::kRingSamples;
  using cardlink::audio::kSampleVoices;
  using cardlink::audio::kVqSlotEmpty;

  SampleDryMixer mixer;
  LoadTone(mixer, 0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "a new session must offer one safe SOF prefill");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "SOF prefill must fill the attack-to-BODY bridge");
  Check(mixer.WantBurst(0) == 0,
        "SOF prefill must be emitted only once");
  GrantEmpty(mixer, 0x00);
  Check(mixer.WantBurst(0) == 0,
        "vq without the requested voice must not permit its refill");
  GrantEmpty(mixer, 0x01, 0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "empty vq must permit the first C4 SOF burst");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "first refill must send the full one-voice grant");
  Check(mixer.WantBurst(0) == 0, "after one refill, wait for vq");

  std::array<uint8_t, kSampleVoices> empty_vq{};
  empty_vq.fill(kVqSlotEmpty);
  std::array<uint8_t, kSampleVoices> nearly_empty{};
  nearly_empty.fill(kVqSlotEmpty);
  nearly_empty[0] = 14; /* filled 1..512 — playhead can already be at 4 */
  mixer.ApplyVoiceQuery(0x01, 0, nearly_empty.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 14u * cardlink::audio::kVqSlotSamples,
        "slot 14 must refill; 4 samples is a hold, not 5 ms of buffer");
  Check(TakeBurst(mixer, 0) == 14u * cardlink::audio::kVqSlotSamples,
        "slot 14 gets its complete safe credit");
  mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  for (unsigned i = 0; i < 5u; ++i) {
    Check(mixer.WantBurst(0) == kBodyBurstMax,
          "each fresh empty-ring vq must permit one bounded refill");
    Check(TakeBurst(mixer, 0) == kBodyBurstMax,
          "fresh vq refill must stay within the per-voice burst cap");
    Check(mixer.WantBurst(0) == 0,
          "one fresh vq must never permit a second refill");
    mixer.ApplyVoiceQuery(0x01, 0, empty_vq.data());
    mixer.ConsumeOutputSamples(0.0);
  }
  std::array<uint8_t, kSampleVoices> fuller_vq{};
  fuller_vq.fill(kVqSlotEmpty);
  fuller_vq[0] = 10;
  mixer.ApplyVoiceQuery(0x01, 0, fuller_vq.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(TakeBurst(mixer, 0) == 10u * cardlink::audio::kVqSlotSamples,
        "fresh fuller-ring vq must still permit exactly one refill");
  mixer.ApplyVoiceQuery(0x01, 0, nearly_empty.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 14u * cardlink::audio::kVqSlotSamples,
        "vq must reconcile an overestimated host fill back to its card bin");
  Check(TakeBurst(mixer, 0) == 14u * cardlink::audio::kVqSlotSamples,
        "reconciled vq must rebuild the real ring reservoir");
  std::array<uint16_t, kSampleVoices> unreflected{};
  unreflected[0] = 400u;
  std::array<uint8_t, kSampleVoices> two_slots{};
  two_slots.fill(kVqSlotEmpty);
  two_slots[0] = 2u;
  mixer.ApplyVoiceQuery(0x01, 0, two_slots.data(), unreflected.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0u,
        "small safe credit must coalesce while the ring is healthy");
  std::array<uint16_t, kSampleVoices> exact_free{};
  exact_free.fill(static_cast<uint16_t>(kRingSamples));
  exact_free[0] = 100u;
  unreflected[0] = 40u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data(), unreflected.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0u,
        "tiny exact USB credit must coalesce instead of emitting a header");
  exact_free[0] = 1200u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data(), unreflected.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 1160u,
        "coalesced USB credit must subtract exact unreflected samples");
  Check(TakeBurst(mixer, 0) == 1160u,
        "coalesced exact credit must be served without slot rounding");
  exact_free[0] = static_cast<uint16_t>(kRingSamples - 4u);
  unreflected[0] = 0u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data(), unreflected.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "exact fill 4 must immediately request a maximum rebuild burst");
  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x01, 0);
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
  Check(mixer.WantBurst(0) == 13u * cardlink::audio::kVqSlotSamples,
        "slot 13 must refill; slot 14 is already 1..512");
  Check(TakeBurst(mixer, 0) == 13u * cardlink::audio::kVqSlotSamples,
        "slot 13 gets its complete safe credit");

  slots.fill(kVqSlotEmpty);
  slots[0] = 12; /* 3072 samples of safe free-space permission */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 12u * cardlink::audio::kVqSlotSamples,
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
  GrantEmpty(mixer, 0x01, 0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "C3 gets one permitted SOF burst");
  slots.fill(kVqSlotEmpty);
  slots[0] = 2; /* 512 samples of safe free-space permission */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0u,
        "healthy low note must coalesce sub-threshold fresh credit");
  slots[0] = 4; /* 1024 samples: enough to amortize one BODY meta */
  mixer.ApplyVoiceQuery(0x01, 0, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 1024u,
        "low note must serve coalesced fresh credit exactly");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() < kSampleVoices,
        "two new notes must both be eligible for safe SOF prefill");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "voice 0 SOF prefill");
  Check(TakeBurst(mixer, 1) == kBodyBurstMax, "voice 1 SOF prefill");
  Check(mixer.HungriestWant() == 0xFF,
        "new notes must await status after their one prefill");
  GrantEmpty(mixer, 0x03, 0);
  Check(mixer.HungriestWant() < kSampleVoices, "two empty notes both need data");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "voice 0 refill");
  Check(mixer.HungriestWant() == 1, "after voice 0 prefill, voice 1 is hungriest");
  slots.fill(kVqSlotEmpty);
  mixer.ApplyVoiceQuery(0x03, 1, slots.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() == 1, "card best must win when it wants data");

  mixer.Silence(0);
  mixer.Silence(1);
  mixer.ConsumeOutputSamples(0.0);

  Check(cardlink::usb::PackMaxSamples(1) == kBodyBurstMax,
        "one-voice PACK must use its full BODY burst cap");
  Check(cardlink::usb::PackMaxSamples(3) == 4720u,
        "three-voice PACK must use all physical wire room");
  Check(cardlink::usb::PackMaxSamples(5) == 4712u,
        "five-voice PACK must use all physical wire room");
  Check(cardlink::usb::PackMaxSamples(8) == 4700u,
        "eight-voice PACK must use all physical wire room");
  Check(cardlink::usb::StreamSustainableSamplesPerMs(3) < 3u * 4u * 48u,
        "three C6 voices from a C4 body must be reported over budget");
  Check(cardlink::usb::StreamSustainableSamplesPerMs(3) >= 3u * 2u * 48u,
        "three C6 voices from a C5 body must fit the service deadline");
  Check(cardlink::usb::StreamSustainableSamplesPerMs(8) >= 8u * 48u,
        "eight C4 voices must fit the one-frame service deadline");
  Check(cardlink::usb::StreamSustainableSamplesPerMs(3) >= 437u,
        "A5+F5+G5 must fit the measured three-voice capacity");
  Check(cardlink::usb::StreamSustainableSamplesPerMs(8) < 437u,
        "the same aggregate rate must be over the eight-voice capacity");

  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(2, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(3, 0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(4, 0, kDefaultBodyRootHz * 2.0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x1F, 0);
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
  GrantEmpty(mixer, 0x07, 2);
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
    Check(by_voice[0] == 674u && by_voice[1] == 1349u &&
              by_voice[2] == 2697u,
          "C4+C5+C6 grants must follow their 1:2:4 consumption rates");
  }

  mixer.AllNotesOff();
  mixer.ConsumeOutputSamples(0.0);
  mixer.SetBodyRootHz(0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 4.0);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz * 4.0);
  mixer.NoteOn(2, 0, kDefaultBodyRootHz * 4.0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x07, 0);
  {
    uint8_t order[kSampleVoices];
    unsigned grants[kSampleVoices]{};
    unsigned by_voice[kSampleVoices]{};
    const unsigned nwant = mixer.WantingVoices(order);
    const unsigned budget = cardlink::usb::PackMaxSamples(nwant);
    Check(nwant == 3, "three C6 voices must all be scheduled");
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "three C6 voices must use the complete service quantum");
    for (unsigned i = 0; i < nwant; ++i) {
      by_voice[order[i]] = grants[i];
    }
    Check(by_voice[0] >= 1573u && by_voice[0] <= 1574u &&
              by_voice[1] >= 1573u && by_voice[1] <= 1574u &&
              by_voice[2] >= 1573u && by_voice[2] <= 1574u &&
              by_voice[0] + by_voice[1] + by_voice[2] == 4720u,
          "three C6 grants must fairly use the catch-up PACK");
    for (unsigned i = 0; i < nwant; ++i) {
      const uint8_t v = order[i];
      std::array<int16_t, kBodyBurstMax> body{};
      bool sof = false;
      uint8_t session = 0;
      Check(mixer.FillBurst(v, body.data(), grants[i], sof, session) ==
                grants[i],
            "shared C6 prefill must emit its fair partial grant");
      Check(mixer.WantBurst(v) == kBodyBurstMax - grants[i],
            "partial chord prefill must remain resumable");
    }
  }

  {
    std::array<double, kSampleVoices> inc{};
    inc[0] = 2.0;
    inc[1] = 2.0;
    inc[2] = 2.0;
    RunNoFaultCase(inc, 3u, 1000u,
                   "three C6 voices with a C5 body must run 10 s without hold/drop",
                   kDefaultBodyRootHz * 2.0);
    inc.fill(1.0);
    RunNoFaultCase(inc, 8u, 1000u,
                   "eight C4 voices must run 10 s without hold/drop");
  }

  return EXIT_SUCCESS;
}
