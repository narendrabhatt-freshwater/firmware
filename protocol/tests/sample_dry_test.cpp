#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/usb/stream_proto.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
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
  std::array<uint16_t, cardlink::audio::kSampleVoices> free_samples{};
  free_samples.fill(
      static_cast<uint16_t>(cardlink::audio::kRingSamples));
  mixer.ApplyVoiceStatus(mask, best, free_samples.data());
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
  constexpr double kStatusPeriodMs = 10.0;
  const double output_frames = 48.0 * kStatusPeriodMs;

  for (uint8_t v = 0; v < nvoices; ++v) {
    mixer.NoteOn(v, 0);
    mixer.ConsumeOutputSamples(0.0);
    mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(1u << v));
  }
  while (mixer.UrgentPending()) {
    const uint8_t voice = mixer.HungriestUacWant(
        cardlink::usb::kStreamUacBodySamples);
    Check(voice < nvoices, "urgent UAC scheduler must select a live voice");
    std::array<int16_t, cardlink::usb::kStreamUacBodySamples> body{};
    bool sof = false;
    uint8_t session = 0u;
    const unsigned got = mixer.FillUacFrame(
        voice, body.data(), body.size(), sof, session);
    Check(got == body.size() && sof,
          "urgent UAC packet must carry 509 tagged BODY samples");
    ring[voice] += got;
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
      if (ring[v] < consume) {
        std::cerr << message << " (underflow q=" << q << " voice="
                  << static_cast<unsigned>(v) << " ring=" << ring[v]
                  << " consume=" << consume << ")\n";
        std::exit(EXIT_FAILURE);
      }
      ring[v] -= consume;
      free_samples[v] = static_cast<uint16_t>(
          ring[v] < kRingSamples ? kRingSamples - ring[v] : 0u);
      const double ms = static_cast<double>(ring[v]) / inc[v];
      if (best == 0xFFu || ms < best_ms) {
        best = v;
        best_ms = ms;
      }
    }

    mixer.ConsumeOutputSamples(output_frames);
    mixer.ApplyVoiceStatus(mask, best, free_samples.data());
    mixer.ConsumeOutputSamples(0.0);
    for (unsigned packet = 0u; packet < 10u; ++packet) {
      const uint8_t v = mixer.HungriestUacWant(
          cardlink::usb::kStreamUacBodySamples);
      if (v >= nvoices) {
        continue;
      }
      std::array<int16_t, cardlink::usb::kStreamUacBodySamples> body{};
      bool sof = false;
      uint8_t session = 0u;
      const unsigned got = mixer.FillUacFrame(
          v, body.data(), body.size(), sof, session);
      if (ring[v] + got > kRingSamples + kRingHeadroom) {
        std::cerr << message << " (overflow q=" << q << " voice="
                  << static_cast<unsigned>(v) << " ring=" << ring[v]
                  << " got=" << got << ")\n";
        std::exit(EXIT_FAILURE);
      }
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

  SampleDryMixer mixer;
  LoadTone(mixer, 0);

  mixer.NoteOn(0, 0);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0,
        "a new session must not refill before vq permission");
  GrantEmpty(mixer, 0x00);
  Check(mixer.WantBurst(0) == 0,
        "vq without the requested voice must not permit its refill");
  GrantEmpty(mixer, 0x01, 0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "empty vq must permit the first C4 SOF burst");
  GrantEmpty(mixer, 0x00);
  Check(mixer.WantBurst(0) == 0u,
        "a newer vq without the voice must revoke an unused older grant");
  GrantEmpty(mixer, 0x01, 0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "first refill must send the full one-voice grant");
  Check(mixer.WantBurst(0) == 0, "after one refill, wait for vq");

  std::array<uint16_t, kSampleVoices> exact_free{};
  exact_free.fill(static_cast<uint16_t>(kRingSamples));
  exact_free[0] = static_cast<uint16_t>(kRingSamples - 4u);
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "a four-sample fill must request a maximum rebuild burst");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "near-empty status must use the bounded exact credit");
  exact_free[0] = static_cast<uint16_t>(kRingSamples);
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  for (unsigned i = 0; i < 5u; ++i) {
    Check(mixer.WantBurst(0) == kBodyBurstMax,
          "each fresh empty-ring vq must permit one bounded refill");
    Check(TakeBurst(mixer, 0) == kBodyBurstMax,
          "fresh vq refill must stay within the per-voice burst cap");
    Check(mixer.WantBurst(0) == 0,
          "one fresh vq must never permit a second refill");
    mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
    mixer.ConsumeOutputSamples(0.0);
  }
  exact_free[0] = 2560u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(TakeBurst(mixer, 0) == 2560u,
        "fresh status must permit one exact refill");
  exact_free[0] = static_cast<uint16_t>(kRingSamples - 4u);
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "status must reconcile an overestimated host fill");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "reconciled status must rebuild the ring reservoir");
  std::array<uint16_t, kSampleVoices> unreflected{};
  unreflected[0] = 400u;
  exact_free[0] = 512u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data(), unreflected.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0u,
        "small safe credit must coalesce while the ring is healthy");
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
  mixer.NoteOn(0, 0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x01, 0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "SOF after exact-credit test");

  mixer.ConsumeOutputSamples(2000.0);
  Check(mixer.WantBurst(0) == 0, "wall-clock must not send without a new vq");

  exact_free.fill(static_cast<uint16_t>(kRingSamples));
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "empty vq must ask for one burst");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "one burst per vq");
  Check(mixer.WantBurst(0) == 0, "already served this vq");

  exact_free[0] = 3328u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 3328u,
        "exact credit below the burst cap must be preserved");
  Check(TakeBurst(mixer, 0) == 3328u,
        "refill must match the exact safe credit");

  exact_free[0] = 3072u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 3072u,
        "any fresh free-space grant must top up the jitter ring");

  exact_free[0] = 0u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0, "zero free-space credit is a hard stop");

  mixer.ConsumeOutputSamples(2000.0);
  exact_free[0] = static_cast<uint16_t>(kRingSamples);
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == kBodyBurstMax,
        "a later empty vq must send again");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x01, 0);
  Check(TakeBurst(mixer, 0) == kBodyBurstMax,
        "C3 gets one permitted SOF burst");
  exact_free[0] = 256u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 0u,
        "healthy low note must coalesce sub-threshold fresh credit");
  exact_free[0] = 512u;
  mixer.ApplyVoiceStatus(0x01, 0, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.WantBurst(0) == 512u,
        "low note must serve coalesced fresh credit exactly");

  mixer.Silence(0);
  mixer.ConsumeOutputSamples(0.0);

  mixer.NoteOn(0, 0);
  mixer.NoteOn(1, 0);
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() == 0xFF,
        "two new notes must both wait for vq permission");
  GrantEmpty(mixer, 0x03, 0);
  Check(mixer.HungriestWant() < kSampleVoices, "two empty notes both need data");
  Check(TakeBurst(mixer, 0) == kBodyBurstMax, "voice 0 refill");
  Check(mixer.HungriestWant() == 1, "after voice 0 prefill, voice 1 is hungriest");
  exact_free.fill(static_cast<uint16_t>(kRingSamples));
  mixer.ApplyVoiceStatus(0x03, 1, exact_free.data());
  mixer.ConsumeOutputSamples(0.0);
  Check(mixer.HungriestWant() == 1, "card best must win when it wants data");

  mixer.Silence(0);
  mixer.Silence(1);
  mixer.ConsumeOutputSamples(0.0);

  Check(cardlink::usb::kStreamUacBodySamples == 509u &&
            cardlink::usb::kStreamBodySamplesPerMs == 509u,
        "direct UAC must carry one tag plus 509 BODY samples per ms");
  Check((cardlink::usb::kStreamTagIdle & cardlink::usb::kStreamTagMask) ==
            cardlink::usb::kStreamTagBase,
        "idle and routed frames must share the direct UAC tag namespace");
  {
    SampleDryMixer chord;
    LoadTone(chord, 0);
    for (uint8_t voice = 0u; voice < kSampleVoices; ++voice) {
      chord.NoteOn(voice, 0u);
    }
    chord.ConsumeOutputSamples(0.0);
    uint8_t seen = 0u;
    for (unsigned frame = 0u; frame < kSampleVoices; ++frame) {
      const uint8_t voice = chord.HungriestUacWant(
          cardlink::usb::kStreamUacBodySamples);
      std::array<int16_t, cardlink::usb::kStreamUacBodySamples> body{};
      bool sof = false;
      uint8_t session = 0u;
      Check(voice < kSampleVoices &&
                chord.FillUacFrame(voice, body.data(), body.size(), sof,
                                   session) == body.size() && sof,
            "each simultaneous voice must receive a direct urgent packet");
      seen = static_cast<uint8_t>(seen | static_cast<uint8_t>(1u << voice));
    }
    Check(seen == 0xFFu,
          "urgent direct packets must interleave all eight chord voices");
  }
  {
    SampleDryMixer pipelined;
    LoadTone(pipelined, 0);
    pipelined.NoteOn(0, 0);
    pipelined.ConsumeOutputSamples(0.0);
    std::array<uint16_t, kSampleVoices> empty{};
    empty.fill(static_cast<uint16_t>(kRingSamples));
    std::array<uint16_t, kSampleVoices> pending{};
    const unsigned expected[] = {4096u, 4064u};
    for (unsigned grant : expected) {
      pipelined.ApplyVoiceStatus(0x01u, 0u, empty.data(), pending.data());
      pipelined.ConsumeOutputSamples(0.0);
      Check(pipelined.WantBurst(0) == grant,
            "pipelined vq must subtract every unacknowledged transfer");
      Check(TakeBurst(pipelined, 0) == grant,
            "pipelined transfer must consume only its exact remaining credit");
      pending[0] = static_cast<uint16_t>(pending[0] + grant);
    }
    pipelined.ApplyVoiceStatus(0x01u, 0u, empty.data(), pending.data());
    pipelined.ConsumeOutputSamples(0.0);
    Check(pipelined.WantBurst(0) == 0u,
          "unacknowledged transfers must never over-reserve the physical ring");
  }
  {
    SampleDryMixer crash;
    LoadTone(crash, 0);
    crash.NoteOn(0, 0);
    crash.ConsumeOutputSamples(0.0);
    GrantEmpty(crash, 0x01u, 0u);
    std::array<int16_t, kBodyBurstMax> body{};
    bool old_sof = false;
    uint8_t old_session = 0u;
    const unsigned old_n = crash.FillBurst(
        0u, body.data(), kBodyBurstMax, old_sof, old_session);
    Check(old_n != 0u && old_sof &&
              crash.BurstIsCurrent(0u, old_session, 0u),
          "first BODY chunk must belong to its original note session");

    crash.NoteOn(0, 0);
    crash.ConsumeOutputSamples(0.0);
    Check(!crash.BurstIsCurrent(0u, old_session, 0u),
          "voice replacement must immediately stale queued old BODY");
    crash.AbortBurst(0u, old_session, 0u, old_n, old_sof);
    crash.CommitBurst(0u, old_session, 0u, old_n, old_sof);
    Check(crash.QueuedSamples(0u) == 0u && !crash.WaitPrefill(0u, 0u),
          "old cancel/ACK accounting must not touch the replacement session");

    GrantEmpty(crash, 0x01u, 0u);
    bool new_sof = false;
    uint8_t new_session = 0u;
    const unsigned new_n = crash.FillBurst(
        0u, body.data(), kBodyBurstMax, new_sof, new_session);
    Check(new_n != 0u && new_sof && new_session != old_session &&
              crash.BurstIsCurrent(0u, new_session, 0u),
          "replacement BODY must become the urgent current session");
    crash.CommitBurst(0u, new_session, 0u, new_n, new_sof);
    Check(crash.WaitPrefill(0u, 0u),
          "only the replacement session may satisfy prefill completion");
  }
  {
    constexpr double kSemitones = 1.33;
    const double pitch_ratio = std::pow(2.0, kSemitones / 12.0);
    const unsigned samples_per_10ms = static_cast<unsigned>(
        std::ceil(48.0 * 10.0 * static_cast<double>(kSampleVoices) *
                  pitch_ratio));
    Check(samples_per_10ms <=
              cardlink::usb::kStreamBodySamplesPerMs * 10u,
          "eight voices at +1.33 semitones must fit direct UAC capacity");
  }
  mixer.NoteOn(0, 0);
  mixer.NoteOn(1, 0);
  mixer.NoteOn(2, 0);
  mixer.NoteOn(3, 0);
  mixer.NoteOn(4, 0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x1F, 0);
  {
    uint8_t order[kSampleVoices];
    const unsigned nwant = mixer.WantingVoices(order);
    Check(nwant == 5, "five C5 notes must all want the SOF burst");
    unsigned grants[kSampleVoices]{};
    const unsigned budget = cardlink::usb::kStreamBodySamplesPerMs * 10u;
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "fair allocator must use the available sample budget");
    for (unsigned i = 0; i < nwant; ++i) {
      Check(grants[i] > 0u,
            "fair allocator must not starve a wanting voice");
    }
  }

  mixer.AllNotesOff();
  mixer.ConsumeOutputSamples(0.0);
  mixer.NoteOn(0, 0);
  mixer.NoteOn(1, 0);
  mixer.NoteOn(2, 0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x07, 2);
  {
    uint8_t order[kSampleVoices];
    const unsigned nwant = mixer.WantingVoices(order);
    unsigned grants[kSampleVoices]{};
    unsigned by_voice[kSampleVoices]{};
    const unsigned budget = cardlink::usb::kStreamBodySamplesPerMs * 10u;
    Check(nwant == 3, "C4+C5+C6 must all want initial BODY");
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "C4+C5+C6 must use the full permitted sample budget");
    for (unsigned i = 0; i < nwant; ++i) {
      by_voice[order[i]] = grants[i];
    }
    Check(std::abs(static_cast<int>(by_voice[0]) -
                       static_cast<int>(by_voice[1])) <= 1 &&
              std::abs(static_cast<int>(by_voice[1]) -
                       static_cast<int>(by_voice[2])) <= 1 &&
              by_voice[0] + by_voice[1] + by_voice[2] == budget,
          "pitch-neutral BODY grants must be shared equally");
  }

  mixer.AllNotesOff();
  mixer.ConsumeOutputSamples(0.0);
  mixer.SetBodyRootHz(0, kDefaultBodyRootHz * 2.0);
  mixer.NoteOn(0, 0);
  mixer.NoteOn(1, 0);
  mixer.NoteOn(2, 0);
  mixer.ConsumeOutputSamples(0.0);
  GrantEmpty(mixer, 0x07, 0);
  {
    uint8_t order[kSampleVoices];
    unsigned grants[kSampleVoices]{};
    unsigned by_voice[kSampleVoices]{};
    const unsigned nwant = mixer.WantingVoices(order);
    const unsigned budget = cardlink::usb::kStreamBodySamplesPerMs * 10u;
    Check(nwant == 3, "three C6 voices must all be scheduled");
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "three C6 voices must use the complete service quantum");
    for (unsigned i = 0; i < nwant; ++i) {
      by_voice[order[i]] = grants[i];
    }
    const unsigned grant_min =
        std::min({by_voice[0], by_voice[1], by_voice[2]});
    const unsigned grant_max =
        std::max({by_voice[0], by_voice[1], by_voice[2]});
    Check(grant_max - grant_min <= 1u &&
              by_voice[0] + by_voice[1] + by_voice[2] == budget,
          "three C6 grants must fairly use ten direct-UAC milliseconds");
    for (unsigned i = 0; i < nwant; ++i) {
      const uint8_t v = order[i];
      std::array<int16_t, kBodyBurstMax> body{};
      bool sof = false;
      uint8_t session = 0;
      Check(mixer.FillBurst(v, body.data(), grants[i], sof, session) ==
                grants[i],
            "shared C6 prefill must emit its fair partial grant");
      Check(mixer.WantBurst(v) == 0u,
            "one vq must never permit a second partial refill");
    }
  }

  {
    std::array<double, kSampleVoices> inc{};
    constexpr double kA4Ratio = 440.0 / kDefaultBodyRootHz;
    inc[0] = 4.0;
    RunNoFaultCase(inc, 1u, 1000u,
                   "one-voice C6 must run 10 s without hold/drop");
    inc[0] = 1.0;
    RunNoFaultCase(inc, 1u, 1000u,
                   "one-voice C4 must run 10 s without hold/drop");
    inc[0] = kA4Ratio;
    RunNoFaultCase(inc, 1u, 1000u,
                   "one-voice A4 must run 10 s without hold/drop");

    inc.fill(0.0);
    inc[0] = 4.0;
    inc[1] = 1.0;
    RunNoFaultCase(inc, 2u, 1000u,
                   "two-voice C6+C4 must run 10 s without hold/drop");
    inc[0] = 4.0;
    inc[1] = kA4Ratio;
    RunNoFaultCase(inc, 2u, 1000u,
                   "two-voice C6+A4 must run 10 s without hold/drop");
    inc[0] = 1.0;
    inc[1] = kA4Ratio;
    RunNoFaultCase(inc, 2u, 1000u,
                   "two-voice C4+A4 must run 10 s without hold/drop");

    inc.fill(0.0);
    inc[0] = 4.0;
    inc[1] = 1.0;
    inc[2] = kA4Ratio;
    RunNoFaultCase(inc, 3u, 1000u,
                   "C6+C4+A4 chord must run 10 s without hold/drop");

    inc.fill(0.0);
    inc[0] = 2.0;
    inc[1] = 2.0;
    inc[2] = 2.0;
    RunNoFaultCase(inc, 3u, 1000u,
                   "three C6 voices with a C5 body must run 10 s without hold/drop",
                   kDefaultBodyRootHz * 2.0);
    inc.fill(1.0);
    RunNoFaultCase(inc, 8u, 1000u,
                   "eight C4 voices must run 10 s without hold/drop");
    inc.fill(std::pow(2.0, 1.33 / 12.0));
    RunNoFaultCase(
        inc, 8u, 1000u,
        "eight voices at +1.33 semitones must run 10 s without hold/drop");
  }

  {
    cardlink::sample::Client client;
    LoadTone(client.Mixer(), 0);
    std::vector<std::string> commands;
    client.SetConsole(
        [&commands](const std::string &cmd) { commands.push_back(cmd); });
    client.SetNoteGate(
        [&commands](const cardlink::sample::NoteRequest &note,
                    cardlink::sample::Client::NoteGateStart start,
                    cardlink::sample::Client::NoteGateDone done) {
          start();
          if (note.note_on) {
            commands.push_back("aw " + std::to_string(note.voice) + " " +
                               std::to_string(note.wave_id));
            commands.push_back("n" + std::to_string(note.voice) + " on " +
                               std::to_string(note.key));
          } else {
            commands.push_back("n" + std::to_string(note.voice) + " off");
          }
          done(true);
          return true;
        });
    const std::array<cardlink::sample::NoteRequest, 2> chord{{
        {0u, 60u, 0u},
        {1u, 64u, 0u},
    }};
    Check(client.NoteOnBatch(chord.data(), chord.size(), 0u),
          "chord must start without waiting for BODY completion");
    Check(commands.size() == 4u && commands[0] == "aw 0 0" &&
              commands[1] == "n0 on 60" && commands[2] == "aw 1 0" &&
              commands[3] == "n1 on 64",
          "RS485 aw/nX ACK must precede each BODY session");
    std::array<int16_t, kBodyBurstMax> urgent_body{};
    cardlink::audio::UrgentBurst first{};
    Check(client.Mixer().FillUrgentBurst(urgent_body.data(),
                                         urgent_body.size(), first) &&
              first.voice == 1u && first.nsamp != 0u,
          "shortest pitch-adjusted attack must meet its SOF deadline first");
    unsigned sof_count = first.sof ? 1u : 0u;
    bool saw_voice0 = false;
    unsigned burst_count = 1u;
    while (client.Mixer().UrgentPending()) {
      cardlink::audio::UrgentBurst chunk{};
      Check(++burst_count < 256u &&
                client.Mixer().FillUrgentBurst(
                    urgent_body.data(), urgent_body.size(), chunk),
            "chunked chord prefill must make progress");
      sof_count += chunk.sof ? 1u : 0u;
      saw_voice0 = saw_voice0 || chunk.voice == 0u;
    }
    Check(sof_count == 2u && saw_voice0,
          "both chord sessions need exactly one SOF plus deadline top-ups");
    client.NoteOff(0u);
    Check(commands.back() == "n0 off" && !client.Mixer().UrgentPending(),
          "RS485 nX off must remain the note-off authority");
    client.SetCrashReleaseMs(0u);
    Check(client.CrashReleaseMs() == 0u && commands.back() == "crash 0",
          "zero-ms crash release must reach the card as a hard-cut test");
    client.SetCrashReleaseMs(255u);
    Check(client.CrashReleaseMs() == 50u && commands.back() == "crash 50",
          "crash release must clamp to the 50-ms test ceiling");
  }

  {
    cardlink::sample::Client gated;
    LoadTone(gated.Mixer(), 0);
    cardlink::sample::Client::NoteGateDone ack;
    cardlink::sample::Client::NoteGateStart start;
    cardlink::sample::NoteRequest gated_note;
    gated.SetNoteGate(
        [&ack, &start, &gated_note](const cardlink::sample::NoteRequest &note,
               cardlink::sample::Client::NoteGateStart begin,
               cardlink::sample::Client::NoteGateDone done) {
          gated_note = note;
          start = std::move(begin);
          ack = std::move(done);
          return true;
        });
    const cardlink::sample::NoteRequest note{0u, 60u, 0u};
    Check(gated.NoteOnBatch(&note, 1u),
          "RS485 note transaction must enter its worker queue");
    Check(!gated.Mixer().UrgentPending(),
          "queued BODY must wait for its RS485 worker");
    start();
    Check(gated.Mixer().UrgentPending(),
          "RS485 worker must launch direct prefill immediately before nX");
    ack(true);
    Check(gated.Mixer().UrgentPending(),
          "nX ACK must preserve its concurrent direct UAC BODY session");
    std::array<int16_t, kBodyBurstMax> tagged_body{};
    cardlink::audio::UrgentBurst tagged{};
    Check(gated_note.session < cardlink::audio::kStreamSessionMod &&
              gated.Mixer().FillUrgentBurst(tagged_body.data(),
                                             tagged_body.size(), tagged) &&
              tagged.session == gated_note.session,
          "nX and its first USB SOF must carry the same reserved session");
  }

  {
    cardlink::sample::Client failed;
    LoadTone(failed.Mixer(), 0);
    failed.SetNoteGate(
        [](const cardlink::sample::NoteRequest &,
           cardlink::sample::Client::NoteGateStart start,
           cardlink::sample::Client::NoteGateDone done) {
          start();
          done(false);
          return true;
        });
    const cardlink::sample::NoteRequest note{0u, 60u, 0u};
    Check(failed.NoteOnBatch(&note, 1u),
          "failed authoritative command must still complete its queued job");
    std::array<int16_t, kBodyBurstMax> body{};
    cardlink::audio::UrgentBurst canceled{};
    Check(!failed.Mixer().FillUrgentBurst(body.data(), body.size(), canceled) &&
              !failed.Mixer().UrgentPending(),
          "failed nX must cancel only its pre-authority BODY session");
  }

  {
    SampleDryMixer bridge;
    LoadTone(bridge, 0);
    bridge.NoteOn(0u, 0u);
    std::array<int16_t, kBodyBurstMax> body{};
    cardlink::audio::UrgentBurst chunk{};
    Check(bridge.FillUrgentBurst(body.data(), 64u, chunk) &&
              chunk.nsamp == 64u && bridge.UrgentPending(),
          "startup bridge must begin with bounded SOF data");
    bridge.EndUrgentPrefill(0x01u);
    Check(bridge.UrgentPending(),
          "first post-nX vq must not truncate the startup runway");
    unsigned total = chunk.nsamp;
    while (bridge.UrgentPending()) {
      Check(bridge.FillUrgentBurst(body.data(), body.size(), chunk),
            "startup bridge must self-complete");
      total += chunk.nsamp;
    }
    Check(total == cardlink::audio::kRingSamples,
          "startup bridge must fill the pitch-neutral safe runway");
  }

  {
    SampleDryMixer urgent;
    LoadTone(urgent, 0);
    urgent.SetCrashReleaseMs(50u);
    const uint8_t old_session =
        urgent.NoteOn(0u, 0u);
    std::array<int16_t, kBodyBurstMax> body{};
    cardlink::audio::UrgentBurst old_note{};
    Check(urgent.FillUrgentBurst(body.data(), body.size(), old_note) &&
              old_note.session == old_session,
          "note-on must assign its session synchronously");
    const uint8_t new_session =
        urgent.NoteOn(0u, 0u);
    cardlink::audio::UrgentBurst replacement{};
    Check(!urgent.FillUrgentBurst(body.data(), body.size(), replacement) &&
              new_session != old_session && !urgent.UrgentPending(),
          "zero safe prefill credit must wait for vq without a USB control record");
  }

  return EXIT_SUCCESS;
}
