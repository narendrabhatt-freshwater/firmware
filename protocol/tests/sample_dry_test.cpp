#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/sample/client.hpp"
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
    mixer.NoteOn(v, 0, body_root_hz * inc[v]);
    mixer.ConsumeOutputSamples(0.0);
    mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(1u << v));
  }
  GrantEmpty(mixer, mask, 0u);
  {
    uint8_t order[kSampleVoices]{};
    unsigned grants[kSampleVoices]{};
    const unsigned nwant = mixer.WantingVoices(order);
    const unsigned budget = cardlink::usb::PackMaxSamples(nwant);
    Check(nwant == nvoices, "first vq must authorize every new voice");
    Check(mixer.AllocateBursts(order, nwant, budget, grants) == budget,
          "first vq must fill one complete UAC PACK");
    for (unsigned i = 0; i < nwant; ++i) {
      std::array<int16_t, kBodyBurstMax> body{};
      bool sof = false;
      uint8_t session = 0u;
      ring[order[i]] = mixer.FillBurst(order[i], body.data(), grants[i], sof,
                                       session);
      Check(ring[order[i]] == grants[i],
            "first vq grant must match its allocated PACK share");
    }
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

    mixer.ConsumeOutputSamples(output_frames);
    mixer.ApplyVoiceStatus(mask, best, free_samples.data());
    mixer.ConsumeOutputSamples(0.0);
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

  SampleDryMixer mixer;
  LoadTone(mixer, 0);

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
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
  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
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

  mixer.NoteOn(0, 0, kDefaultBodyRootHz * 0.5);
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

  mixer.NoteOn(0, 0, kDefaultBodyRootHz);
  mixer.NoteOn(1, 0, kDefaultBodyRootHz);
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

  Check(cardlink::usb::PackMaxSamples(1) == kBodyBurstMax,
        "one-voice PACK must use its full BODY burst cap");
  Check(cardlink::usb::PackMaxSamples(3) == 4782u,
        "three-voice PACK must use all physical wire room");
  Check(cardlink::usb::PackMaxSamples(5) == 4774u,
        "five-voice PACK must use all physical wire room");
  Check(cardlink::usb::PackMaxSamples(8) == 4762u,
        "eight-voice PACK must use all physical wire room");
  Check(cardlink::usb::NextPackSequence(0xFFFEu) == 0u,
        "PACK sequence must reserve 0xffff as the no-ack sentinel");
  {
    constexpr uint8_t kCrcVector[] = {'1', '2', '3', '4', '5',
                                      '6', '7', '8', '9'};
    Check(cardlink::usb::StreamCrc32(kCrcVector, sizeof kCrcVector) ==
              0xCBF43926u,
          "PACK CRC32 must match the standard check vector");
  }
  {
    SampleDryMixer pipelined;
    LoadTone(pipelined, 0);
    pipelined.NoteOn(0, 0, kDefaultBodyRootHz);
    pipelined.ConsumeOutputSamples(0.0);
    std::array<uint16_t, kSampleVoices> empty{};
    empty.fill(static_cast<uint16_t>(kRingSamples));
    std::array<uint16_t, kSampleVoices> pending{};
    const unsigned expected[] = {4096u, 4096u, 4048u};
    for (unsigned grant : expected) {
      pipelined.ApplyVoiceStatus(0x01u, 0u, empty.data(), pending.data());
      pipelined.ConsumeOutputSamples(0.0);
      Check(pipelined.WantBurst(0) == grant,
            "pipelined vq must subtract every unacknowledged PACK");
      Check(TakeBurst(pipelined, 0) == grant,
            "pipelined PACK must consume only its exact remaining credit");
      pending[0] = static_cast<uint16_t>(pending[0] + grant);
    }
    pipelined.ApplyVoiceStatus(0x01u, 0u, empty.data(), pending.data());
    pipelined.ConsumeOutputSamples(0.0);
    Check(pipelined.WantBurst(0) == 0u,
          "unacknowledged PACKs must never over-reserve the physical ring");
  }
  {
    constexpr double kSemitones = 1.33;
    const double pitch_ratio = std::pow(2.0, kSemitones / 12.0);
    const unsigned samples_per_10ms = static_cast<unsigned>(
        std::ceil(48.0 * 10.0 * static_cast<double>(kSampleVoices) *
                  pitch_ratio));
    const unsigned wire_bytes = cardlink::usb::kStreamHdrSize +
        kSampleVoices * cardlink::usb::kStreamBodyMetaSize +
        2u * samples_per_10ms;
    Check(wire_bytes <= cardlink::usb::kStreamFrameMax,
          "eight voices at +1.33 semitones must fit one 10 ms iso window");
  }
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
    Check(cardlink::usb::PackMaxSamples(nwant) == 4774u,
          "16-bit UAC PACK capacity must include its framing cost");
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
    Check(by_voice[0] >= 683u && by_voice[0] <= 684u &&
              by_voice[1] >= 1366u && by_voice[1] <= 1367u &&
              by_voice[2] >= 2732u && by_voice[2] <= 2733u &&
              by_voice[0] + by_voice[1] + by_voice[2] == 4782u,
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
    Check(by_voice[0] == 1594u && by_voice[1] == 1594u &&
              by_voice[2] == 1594u &&
              by_voice[0] + by_voice[1] + by_voice[2] == 4782u,
          "three C6 grants must fairly use the catch-up PACK");
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
    const std::array<cardlink::sample::NoteRequest, 2> chord{{
        {0u, 261.625565, 0u},
        {1u, 329.627557, 0u},
    }};
    Check(client.NoteOnBatch(chord.data(), chord.size(), 0u),
          "chord must start without waiting for BODY completion");
    Check(commands.size() == 4u && commands[0] == "aw 0 0" &&
              commands[1] == "aw 1 0" && commands[2].rfind("n0 ", 0u) == 0u &&
              commands[3].rfind("n1 ", 0u) == 0u,
          "audible chord commands must follow session setup immediately");
  }

  return EXIT_SUCCESS;
}
