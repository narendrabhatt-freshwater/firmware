#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/usb/stream_proto.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

namespace cardlink {
namespace audio {

namespace {
constexpr unsigned kUacPacketSamples = cardlink::usb::kStreamUacBodySamples;
/* A vq snapshot can race the callback plus CoreAudio's queued endpoint data.
 * Reserve eight complete millisecond packets across a voice replacement. */
constexpr unsigned kUacInFlightHeadroom = 8u * kUacPacketSamples;
}

SampleDryMixer::SampleDryMixer()
{
  for (auto &h : root_hz_) {
    h.store(kDefaultBodyRootHz, std::memory_order_relaxed);
  }
  for (auto &o : oneshot_) {
    o.store(false, std::memory_order_relaxed);
  }
  for (auto &n : attack_len_) {
    n.store(0, std::memory_order_relaxed);
  }
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    sent_[i].store(0, std::memory_order_relaxed);
    committed_[i].store(0, std::memory_order_relaxed);
    next_session_[i].store(0, std::memory_order_relaxed);
    live_[i].store(false, std::memory_order_relaxed);
    live_wave_[i].store(0xFFFFu, std::memory_order_relaxed);
    vq_fill_[i].store(0, std::memory_order_relaxed);
    vq_fill_max_[i].store(0, std::memory_order_relaxed);
    vq_free_[i].store(static_cast<uint16_t>(kRingSamples),
                      std::memory_order_relaxed);
    vq_unreflected_[i].store(0, std::memory_order_relaxed);
  }
}

bool SampleDryMixer::LoadBodyFile(uint16_t wave_id,
                                  const std::string &path, std::string &err)
{
  if (wave_id >= bodies_.size()) {
    err = "wave_id out of range";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open " + path;
    return false;
  }
  std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  if (raw.size() < 2 || (raw.size() & 1u) != 0u) {
    err = "body must be even int16 LE bytes";
    return false;
  }
  std::vector<int16_t> tmp(raw.size() / 2);
  for (size_t i = 0; i < tmp.size(); ++i) {
    tmp[i] = static_cast<int16_t>(
        static_cast<uint8_t>(raw[i * 2]) |
        (static_cast<uint16_t>(static_cast<uint8_t>(raw[i * 2 + 1])) << 8));
  }
  return SetBody(wave_id, tmp.data(), tmp.size(), err);
}

bool SampleDryMixer::WaveInUse(uint16_t wave_id) const
{
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    if (live_[i].load(std::memory_order_acquire) &&
        live_wave_[i].load(std::memory_order_acquire) == wave_id) {
      return true;
    }
  }
  return false;
}

bool SampleDryMixer::SetBody(uint16_t wave_id, const int16_t *data, size_t nsamp,
                             std::string &err)
{
  if (wave_id >= bodies_.size()) {
    err = "wave_id out of range";
    return false;
  }
  if (data == nullptr || nsamp == 0) {
    err = "empty body";
    return false;
  }
  if (WaveInUse(wave_id)) {
    err = "body in use";
    return false;
  }
  bodies_[wave_id].assign(data, data + nsamp);
  return true;
}

bool SampleDryMixer::HasBody(uint16_t wave_id) const
{
  return wave_id < bodies_.size() && !bodies_[wave_id].empty();
}

void SampleDryMixer::SetAttackLen(uint16_t wave_id, unsigned nsamp)
{
  if (wave_id >= attack_len_.size()) {
    return;
  }
  if (nsamp > kAttackSamples) {
    nsamp = kAttackSamples;
  }
  attack_len_[wave_id].store(nsamp, std::memory_order_release);
}

unsigned SampleDryMixer::AttackLen(uint16_t wave_id) const
{
  if (wave_id >= attack_len_.size()) {
    return 0;
  }
  return attack_len_[wave_id].load(std::memory_order_acquire);
}

void SampleDryMixer::SetBodyRootHz(uint16_t wave_id, double root_hz)
{
  if (wave_id >= root_hz_.size() || !(root_hz > 0.0) || !std::isfinite(root_hz)) {
    return;
  }
  root_hz_[wave_id].store(root_hz, std::memory_order_release);
}

double SampleDryMixer::BodyRootHz(uint16_t wave_id) const
{
  if (wave_id >= root_hz_.size()) {
    return kDefaultBodyRootHz;
  }
  return root_hz_[wave_id].load(std::memory_order_acquire);
}

void SampleDryMixer::SetBodyOneshot(uint16_t wave_id, bool oneshot)
{
  if (wave_id >= oneshot_.size()) {
    return;
  }
  oneshot_[wave_id].store(oneshot, std::memory_order_release);
}

bool SampleDryMixer::BodyOneshot(uint16_t wave_id) const
{
  if (wave_id >= oneshot_.size()) {
    return false;
  }
  return oneshot_[wave_id].load(std::memory_order_acquire);
}

bool SampleDryMixer::LoadRootsFile(const std::string &path, std::string &err)
{
  std::ifstream in(path);
  if (!in) {
    err = "cannot open " + path;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream iss(line);
    unsigned id = 0;
    double hz = 0.0;
    std::string mode;
    if (!(iss >> id >> hz)) {
      continue;
    }
    if (id < root_hz_.size() && hz > 0.0 && std::isfinite(hz)) {
      SetBodyRootHz(static_cast<uint16_t>(id), hz);
    }
    if (iss >> mode) {
      bool one = (mode == "oneshot" || mode == "one-shot" || mode == "1");
      if (id < oneshot_.size()) {
        SetBodyOneshot(static_cast<uint16_t>(id), one);
      }
    }
  }
  return true;
}

double SampleDryMixer::IncOf(const Voice &v) const
{
  (void)v;
  return 1.0;
}

double SampleDryMixer::Fade0Of(unsigned attack_len)
{
  if (attack_len > kCrossfadeSamples) {
    return static_cast<double>(attack_len - kCrossfadeSamples);
  }
  return 0.0;
}

bool SampleDryMixer::BodyDraining(const Voice &v)
{
  return v.phase >= Fade0Of(v.attack_len);
}

void SampleDryMixer::ConsumeOutputSamples(double nframes)
{
  DrainCmds();
  PullVq();
  if (nframes <= 0.0) {
    return;
  }
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    auto &v = voices_[i];
    if (!v.active) {
      continue;
    }
    const double inc = IncOf(v);
    v.phase += inc * nframes;
    if (!BodyDraining(v)) {
      continue;
    }
    v.queued -= inc * nframes;
    if (v.queued < 0.0) {
      v.queued = 0.0;
    }
  }
}

void SampleDryMixer::PullVq()
{
  const uint32_t seq = vq_seq_.load(std::memory_order_acquire);
  const uint8_t mask = vq_mask_.load(std::memory_order_relaxed);
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    auto &v = voices_[i];
    if (!v.active || v.vq_seen == seq) {
      continue;
    }
    v.vq_seen = seq;
    if ((mask & static_cast<uint8_t>(1u << i)) == 0u) {
      /* A newer status supersedes any unused older grant. No mask bit means
       * this voice has no permission in this vq cycle. */
      v.vq_have = false;
      v.vq_free = 0u;
      v.sent_this_vq = 0u;
      continue;
    }
    unsigned projected = 0u;
    if (v.queued > 0.0) {
      projected = static_cast<unsigned>(std::ceil(v.queued));
      if (projected > kRingSamples) {
        projected = kRingSamples;
      }
    }
    const unsigned reported_fill_min =
        vq_fill_[i].load(std::memory_order_relaxed);
    unsigned reported_fill_max =
        vq_fill_max_[i].load(std::memory_order_relaxed);
    const unsigned unreflected =
        vq_unreflected_[i].load(std::memory_order_relaxed);
    const unsigned reported_fill_min_with_out =
        std::min(kRingSamples, reported_fill_min + unreflected);
    reported_fill_max =
        std::min(kRingSamples, reported_fill_max + unreflected);
    if (projected < reported_fill_min_with_out) {
      projected = reported_fill_min_with_out;
    } else if (projected > reported_fill_max) {
      projected = reported_fill_max;
    }
    v.queued = static_cast<double>(projected);
    const unsigned reported_free =
        vq_free_[i].load(std::memory_order_relaxed);
    const unsigned projected_free = kRingSamples - projected;
    const unsigned reported_credit =
        (reported_free > unreflected) ? (reported_free - unreflected) : 0u;
    /* vq is the permission and its free-space bin is authoritative. Account
     * exactly for the concurrent OUT transfer if it raced the snapshot. */
    v.vq_free = std::min(reported_credit, projected_free);
    v.vq_card_fill = projected;
    v.sent_this_vq = 0u;
    v.vq_have = true;
  }
}

double SampleDryMixer::RemainingMs(const Voice &v) const
{
  if (!v.vq_have) {
    return 0.0;
  }
  const double inc = IncOf(v);
  const double rate = static_cast<double>(kSampleRateHz) * inc;
  if (rate <= 0.0) {
    return 0.0;
  }
  const unsigned projected = std::min(
      kRingSamples, v.vq_card_fill + v.sent_this_vq);
  return 1000.0 * static_cast<double>(projected) / rate;
}

unsigned SampleDryMixer::WantBurst(uint8_t voice) const
{
  if (voice >= kSampleVoices || !voices_[voice].active) {
    return 0;
  }
  const auto &v = voices_[voice];
  if (!v.vq_have) {
    return 0;
  }
  if (v.sent_this_vq != 0u || v.vq_free == 0u) {
    return 0;
  }
  /* A fresh vq is both permission and safe credit. Fill the jitter ring up to
   * the reported credit, bounded by the per-voice burst limit. */
  unsigned n = v.vq_free;
  if (n > kBodyBurstMax) {
    n = kBodyBurstMax;
  }
  if (n < kMinBurst && RemainingMs(v) >= 1.0) {
    return 0;
  }
  return n;
}

unsigned SampleDryMixer::WantUacSamples(uint8_t voice) const
{
  if (voice >= kSampleVoices || !voices_[voice].active) {
    return 0u;
  }
  const auto &v = voices_[voice];
  if (v.urgent_pending) {
    /* The final indivisible frame may exceed the target by at most eight
     * samples, well inside the ring's interpolation headroom. */
    return v.urgent_budget < kUacPacketSamples
               ? kUacPacketSamples
               : v.urgent_budget;
  }
  if (!v.vq_have || v.vq_free <= kUacInFlightHeadroom) {
    return 0u;
  }
  const unsigned safe_credit = v.vq_free - kUacInFlightHeadroom;
  if (v.sent_this_vq >= safe_credit) {
    return 0u;
  }
  const unsigned n = safe_credit - v.sent_this_vq;
  if (n < kMinBurst && RemainingMs(v) >= 1.0) {
    return 0u;
  }
  return n;
}

uint8_t SampleDryMixer::HungriestUacWant(unsigned frame_samples)
{
  DrainCmds();
  PullVq();
  uint8_t urgent[kSampleVoices]{};
  const unsigned nurgent = UrgentVoices(urgent);
  uint8_t urgent_best = 0xFFu;
  double least_runway_ms = 0.0;
  double urgent_best_inc = 0.0;
  uint32_t urgent_best_epoch = 0u;
  for (unsigned i = 0u; i < nurgent; ++i) {
    const uint8_t voice = urgent[i];
    if (WantUacSamples(voice) < frame_samples) {
      continue;
    }
    const double inc = IncOf(voices_[voice]);
    const double rate = static_cast<double>(kSampleRateHz) * inc;
    const double runway_ms = rate > 0.0
                                  ? 1000.0 * voices_[voice].queued / rate
                                  : 0.0;
    /* Interleave simultaneous note starts by buffered playback time. Filling
     * an entire 25 ms reservoir newest-first can leave later chord voices
     * without even their first SOF before their attacks reach BODY. */
    if (urgent_best == 0xFFu || runway_ms < least_runway_ms ||
        (runway_ms == least_runway_ms &&
         (inc > urgent_best_inc ||
          (inc == urgent_best_inc &&
           voices_[voice].urgent_epoch < urgent_best_epoch)))) {
      urgent_best = voice;
      least_runway_ms = runway_ms;
      urgent_best_inc = inc;
      urgent_best_epoch = voices_[voice].urgent_epoch;
    }
  }
  if (urgent_best != 0xFFu) {
    return urgent_best;
  }
  uint8_t best = 0xFFu;
  double best_time = 0.0;
  double best_inc = 0.0;
  for (uint8_t voice = 0u; voice < kSampleVoices; ++voice) {
    if (WantUacSamples(voice) < frame_samples) {
      continue;
    }
    const double time = RemainingMs(voices_[voice]);
    const double inc = IncOf(voices_[voice]);
    if (best == 0xFFu || time < best_time ||
        (time == best_time && inc > best_inc)) {
      best = voice;
      best_time = time;
      best_inc = inc;
    }
  }
  return best;
}

uint8_t SampleDryMixer::HungriestWant() const
{
  uint8_t order[kSampleVoices];
  const unsigned n = WantingVoices(order);
  if (n == 0) {
    return 0xFF;
  }
  return order[0];
}

unsigned SampleDryMixer::WantingVoices(uint8_t *dst) const
{
  uint8_t tmp[kSampleVoices];
  unsigned n = 0;
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    if (WantBurst(i) > 0) {
      tmp[n++] = i;
    }
  }
  for (unsigned i = 1; i < n; ++i) {
    const uint8_t v = tmp[i];
    const double t = RemainingMs(voices_[v]);
    const double inc = IncOf(voices_[v]);
    unsigned j = i;
    while (j > 0) {
      const double t0 = RemainingMs(voices_[tmp[j - 1u]]);
      const double inc0 = IncOf(voices_[tmp[j - 1u]]);
      if (t0 < t || (t0 == t && inc0 >= inc)) {
        break;
      }
      tmp[j] = tmp[j - 1u];
      --j;
    }
    tmp[j] = v;
  }
  const uint8_t card_best = vq_best_.load(std::memory_order_relaxed);
  if (card_best < kSampleVoices) {
    for (unsigned i = 0; i < n; ++i) {
      if (tmp[i] != card_best) {
        continue;
      }
      for (unsigned j = i; j > 0; --j) {
        tmp[j] = tmp[j - 1u];
      }
      tmp[0] = card_best;
      break;
    }
  }
  if (dst != nullptr) {
    for (unsigned i = 0; i < n; ++i) {
      dst[i] = tmp[i];
    }
  }
  return n;
}

unsigned SampleDryMixer::AllocateBursts(const uint8_t *voices,
                                        unsigned nvoices, unsigned budget,
                                        unsigned *grants) const
{
  if (voices == nullptr || grants == nullptr || nvoices == 0u ||
      nvoices > kSampleVoices || budget == 0u) {
    return 0u;
  }

  unsigned caps[kSampleVoices]{};
  double weights[kSampleVoices]{};
  for (unsigned i = 0; i < nvoices; ++i) {
    grants[i] = 0u;
    const uint8_t voice = voices[i];
    caps[i] = WantBurst(voice);
    if (voice < kSampleVoices && voices_[voice].active && caps[i] != 0u) {
      weights[i] = IncOf(voices_[voice]);
    }
  }

  /* Weighted fair queueing at sample granularity. One UAC window holds
   * fewer than 4800 samples, so this bounded loop is small and
   * avoids rounding one voice down to zero. The input order remains the
   * tie-breaker (vq best first). */
  unsigned used = 0u;
  while (used < budget) {
    unsigned best = nvoices;
    double best_score = 0.0;
    for (unsigned i = 0; i < nvoices; ++i) {
      if (weights[i] <= 0.0 || grants[i] >= caps[i]) {
        continue;
      }
      const double score = static_cast<double>(grants[i]) / weights[i];
      if (best == nvoices || score < best_score) {
        best = i;
        best_score = score;
      }
    }
    if (best == nvoices) {
      break;
    }
    ++grants[best];
    ++used;
  }
  return used;
}

unsigned SampleDryMixer::SourceDemandSamples(double interval_ms) const
{
  if (!(interval_ms > 0.0) || !std::isfinite(interval_ms)) {
    return 0u;
  }
  double total = 0.0;
  for (const auto &voice : voices_) {
    if (voice.active) {
      total += IncOf(voice) *
               (static_cast<double>(kSampleRateHz) / 1000.0) * interval_ms;
    }
  }
  return static_cast<unsigned>(std::ceil(total));
}

unsigned SampleDryMixer::FillBurst(uint8_t voice, int16_t *dst, unsigned max_n,
                                   bool &sof, uint8_t &session)
{
  sof = false;
  session = 0;
  if (dst == nullptr || voice >= kSampleVoices || max_n == 0) {
    return 0;
  }
  DrainCmds();
  PullVq();
  auto &v = voices_[voice];
  if (!v.active) {
    return 0;
  }
  unsigned n = max_n;
  if (n > kBodyBurstMax) {
    n = kBodyBurstMax;
  }
  const unsigned want = WantBurst(voice);
  if (want == 0) {
    return 0;
  }
  if (n > want) {
    n = want;
  }
  session = v.session;
  sof = v.sof_pending;
  v.sof_pending = false;
  for (unsigned i = 0; i < n; ++i) {
    dst[i] = NextBody(v);
  }
  v.queued += static_cast<double>(n);
  v.sent_this_vq += n;
  const unsigned sent = sent_[voice].load(std::memory_order_relaxed) + n;
  sent_[voice].store(sent, std::memory_order_release);
  if (!v.active) {
    live_[voice].store(false, std::memory_order_release);
    live_wave_[voice].store(0xFFFFu, std::memory_order_release);
  }
  return n;
}

unsigned SampleDryMixer::FillUacFrame(uint8_t voice, int16_t *dst,
                                      unsigned max_n, bool &sof,
                                      uint8_t &session)
{
  sof = false;
  session = 0u;
  if (dst == nullptr || voice >= kSampleVoices || max_n == 0u) {
    return 0u;
  }
  DrainCmds();
  PullVq();
  auto &v = voices_[voice];
  if (!v.active || WantUacSamples(voice) < max_n) {
    return 0u;
  }
  const bool urgent = v.urgent_pending;
  session = v.session;
  /* Repeat SOF on every urgent frame. Frames sent before nX are harmless;
   * the first frame accepted after authority starts the session and later
   * repeated SOFs append to it. */
  sof = urgent;
  for (unsigned i = 0u; i < max_n; ++i) {
    dst[i] = NextBody(v);
  }
  v.queued += static_cast<double>(max_n);
  if (urgent) {
    /* A vq snapshot can arrive while the concurrent startup runway is still
     * being emitted. Charge those packets to that snapshot too, otherwise
     * the first steady refill can reserve the same ring space a second time. */
    if (v.vq_have) {
      v.sent_this_vq += max_n;
    }
    if (v.urgent_budget > max_n) {
      v.urgent_budget -= max_n;
    } else {
      v.urgent_budget = 0u;
      v.urgent_pending = false;
      urgent_ready_.fetch_sub(1u, std::memory_order_release);
      v.sof_pending = false;
    }
  } else {
    v.sent_this_vq += max_n;
  }
  sent_[voice].fetch_add(max_n, std::memory_order_release);
  if (!v.active) {
    live_[voice].store(false, std::memory_order_release);
    live_wave_[voice].store(0xFFFFu, std::memory_order_release);
  }
  return max_n;
}

bool SampleDryMixer::BurstIsCurrent(uint8_t voice, uint8_t session,
                                    uint16_t wave_id) const
{
  return voice < kSampleVoices && voices_[voice].active &&
         voices_[voice].session == session &&
         voices_[voice].wave_id == wave_id;
}

void SampleDryMixer::AbortBurst(uint8_t voice, uint8_t session,
                                uint16_t wave_id, unsigned nsamp, bool sof)
{
  if (nsamp == 0 || !BurstIsCurrent(voice, session, wave_id)) {
    return;
  }
  auto &v = voices_[voice];
  if (sof) {
    v.sof_pending = true;
  }
  if (v.queued > static_cast<double>(nsamp)) {
    v.queued -= static_cast<double>(nsamp);
  } else {
    v.queued = 0.0;
  }
  if (v.sent_this_vq > nsamp) {
    v.sent_this_vq -= nsamp;
  } else {
    v.sent_this_vq = 0u;
  }
  v.cursor -= static_cast<double>(nsamp);
  const size_t nbody = (v.wave_id < bodies_.size()) ? bodies_[v.wave_id].size() : 0;
  if (nbody > 0 && v.cursor < 0.0) {
    const bool oneshot = oneshot_[v.wave_id].load(std::memory_order_relaxed);
    if (oneshot) {
      v.cursor = 0.0;
    } else {
      const double n_d = static_cast<double>(nbody);
      v.cursor = std::fmod(v.cursor, n_d);
      if (v.cursor < 0.0) {
        v.cursor += n_d;
      }
    }
  }
  const unsigned sent = sent_[voice].load(std::memory_order_relaxed);
  const unsigned restored = sent > nsamp ? sent - nsamp : 0u;
  sent_[voice].store(restored, std::memory_order_release);
}

void SampleDryMixer::AbortBurst(uint8_t voice, unsigned nsamp, bool sof)
{
  if (voice >= kSampleVoices) {
    return;
  }
  AbortBurst(voice, voices_[voice].session, voices_[voice].wave_id,
             nsamp, sof);
}

void SampleDryMixer::CommitBurst(uint8_t voice, uint8_t session,
                                 uint16_t wave_id, unsigned nsamp, bool sof)
{
  if (nsamp == 0u || !BurstIsCurrent(voice, session, wave_id)) {
    return;
  }
  (void)sof;
  committed_[voice].fetch_add(nsamp, std::memory_order_release);
}

void SampleDryMixer::CommitBurst(uint8_t voice, unsigned nsamp, bool sof)
{
  if (voice >= kSampleVoices) {
    return;
  }
  CommitBurst(voice, voices_[voice].session, voices_[voice].wave_id,
              nsamp, sof);
}

bool SampleDryMixer::Post(const Cmd &c)
{
  std::lock_guard<std::mutex> lock(cmd_post_mutex_);
  const uint32_t w = cmd_wr_.load(std::memory_order_relaxed);
  const uint32_t r = cmd_rd_.load(std::memory_order_acquire);
  if ((w - r) >= kCmdCap) {
    return false;
  }
  cmds_[w % kCmdCap] = c;
  cmd_wr_.store(w + 1u, std::memory_order_release);
  return true;
}

void SampleDryMixer::ApplyCmd(const Cmd &c)
{
  if (c.kind == CmdKind::AllOff) {
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      const uint8_t session = voices_[i].session;
      voices_[i] = Voice{};
      voices_[i].session = session;
      sent_[i].store(0, std::memory_order_release);
      committed_[i].store(0, std::memory_order_release);
      live_[i].store(false, std::memory_order_release);
      live_wave_[i].store(0xFFFFu, std::memory_order_release);
    }
    vq_mask_.store(0, std::memory_order_relaxed);
    vq_best_.store(0xFF, std::memory_order_relaxed);
    urgent_ready_.store(0u, std::memory_order_release);
    return;
  }
  if (c.voice >= kSampleVoices) {
    return;
  }
  auto &v = voices_[c.voice];
  if (c.kind == CmdKind::Cancel) {
    if (v.active && v.session == c.session && v.wave_id == c.wave_id) {
      if (v.urgent_pending) {
        urgent_ready_.fetch_sub(1u, std::memory_order_relaxed);
      }
      const uint8_t session = v.session;
      v = Voice{};
      v.session = session;
      sent_[c.voice].store(0, std::memory_order_release);
      committed_[c.voice].store(0, std::memory_order_release);
      live_[c.voice].store(false, std::memory_order_release);
      live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    }
    return;
  }
  if (c.kind == CmdKind::Silence) {
    if (v.urgent_pending) {
      urgent_ready_.fetch_sub(1u, std::memory_order_relaxed);
    }
    const uint8_t session = v.session;
    v = Voice{};
    v.session = session;
    sent_[c.voice].store(0, std::memory_order_release);
    committed_[c.voice].store(0, std::memory_order_release);
    live_[c.voice].store(false, std::memory_order_release);
    live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    return;
  }
  if (c.wave_id >= bodies_.size() || bodies_[c.wave_id].empty()) {
    live_[c.voice].store(false, std::memory_order_release);
    live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    return;
  }
  const bool was_active = v.active;
  const double old_inc = was_active ? 16.0 : 0.0;
  if (v.urgent_pending) {
    urgent_ready_.fetch_sub(1u, std::memory_order_relaxed);
  }
  v = Voice{};
  v.active = true;
  v.sof_pending = true;
  v.urgent_pending = true;
  v.session = c.session;
  v.wave_id = c.wave_id;
  v.attack_elapsed_ms = std::max(0.0, c.attack_elapsed_ms);
  v.phase = 0.0;
  v.queued = 0.0;
  /* Join index is this wave's committed head, not the voice slot.
   * MIDI plays wave_id = key on whatever voice was allocated. */
  v.attack_len = attack_len_[c.wave_id].load(std::memory_order_acquire);
  unsigned reserve = 0u;
  if (was_active) {
    reserve = static_cast<unsigned>(std::ceil(
        old_inc * static_cast<double>(kSampleRateHz) *
        static_cast<double>(crash_release_ms_.load(std::memory_order_relaxed)) /
        1000.0)) + 2u;
    reserve = std::min(reserve, kRingSamples);
  }
  /* Queue a complete runway before normal exact-credit UAC packets follow.
   * The reservation remains the hard cap while the direct packets bridge
   * host callback and status timing. */
  const unsigned safe_capacity = kRingSamples - reserve;
  v.urgent_budget = safe_capacity;
  v.urgent_epoch = c.epoch;
  urgent_ready_.fetch_add(1u, std::memory_order_release);
  v.vq_seen = vq_seq_.load(std::memory_order_acquire);
  sent_[c.voice].store(0, std::memory_order_release);
  committed_[c.voice].store(0, std::memory_order_release);
  live_wave_[c.voice].store(c.wave_id, std::memory_order_release);
  live_[c.voice].store(true, std::memory_order_release);
}

void SampleDryMixer::DrainCmds()
{
  uint32_t r = cmd_rd_.load(std::memory_order_relaxed);
  const uint32_t w = cmd_wr_.load(std::memory_order_acquire);
  while (r != w) {
    ApplyCmd(cmds_[r % kCmdCap]);
    ++r;
  }
  cmd_rd_.store(r, std::memory_order_release);
}

uint8_t SampleDryMixer::NoteOn(uint8_t voice, uint16_t wave_id,
                               double attack_elapsed_ms)
{
  const uint8_t session = ReserveSession(voice);
  return NoteOnSession(voice, wave_id, session, attack_elapsed_ms);
}

uint8_t SampleDryMixer::ReserveSession(uint8_t voice)
{
  if (voice >= kSampleVoices) {
    return 0xFFu;
  }
  uint8_t session = next_session_[voice].load(std::memory_order_relaxed);
  session = static_cast<uint8_t>((session + 1u) % kStreamSessionMod);
  next_session_[voice].store(session, std::memory_order_release);
  return session;
}

uint8_t SampleDryMixer::NoteOnSession(uint8_t voice, uint16_t wave_id,
                                      uint8_t session,
                                      double attack_elapsed_ms)
{
  if (voice >= kSampleVoices || wave_id >= bodies_.size()) {
    return 0xFFu;
  }
  if (session >= kStreamSessionMod) {
    return 0xFFu;
  }
  /* Mark in-use before the BODY thread drains the command so SetBody
   * cannot reallocate a table FillBurst is about to read. */
  live_wave_[voice].store(wave_id, std::memory_order_release);
  live_[voice].store(true, std::memory_order_release);
  Cmd c;
  c.kind = CmdKind::On;
  c.voice = voice;
  c.wave_id = wave_id;
  c.attack_elapsed_ms = attack_elapsed_ms;
  c.session = session;
  c.epoch = note_epoch_.fetch_add(1u, std::memory_order_relaxed) + 1u;
  if (!Post(c)) {
    live_[voice].store(false, std::memory_order_release);
    live_wave_[voice].store(0xFFFFu, std::memory_order_release);
    return 0xFFu;
  }
  return session;
}

void SampleDryMixer::CancelSession(uint8_t voice, uint8_t session,
                                   uint16_t wave_id)
{
  if (voice >= kSampleVoices || session >= kStreamSessionMod) {
    return;
  }
  Cmd c;
  c.kind = CmdKind::Cancel;
  c.voice = voice;
  c.session = session;
  c.wave_id = wave_id;
  (void)Post(c);
}

void SampleDryMixer::SetCrashReleaseMs(uint8_t release_ms)
{
  crash_release_ms_.store(std::min<uint8_t>(release_ms, 50u),
                          std::memory_order_release);
}

bool SampleDryMixer::UrgentPending() const
{
  return cmd_wr_.load(std::memory_order_acquire) !=
             cmd_rd_.load(std::memory_order_acquire) ||
         urgent_ready_.load(std::memory_order_acquire) != 0u;
}

bool SampleDryMixer::FillUrgentBurst(int16_t *dst, unsigned max_n,
                                     UrgentBurst &info)
{
  uint8_t order[kSampleVoices]{};
  if (UrgentVoices(order) == 0u) {
    return false;
  }
  return FillUrgentBurstForVoice(order[0], dst, max_n,
                                 kUrgentQuantumMaxMs, info);
}

unsigned SampleDryMixer::UrgentVoices(uint8_t *dst)
{
  if (dst == nullptr) {
    return 0u;
  }
  DrainCmds();
  uint8_t used = 0u;
  unsigned count = 0u;
  while (count < kSampleVoices) {
    uint8_t best = 0xFFu;
    uint32_t selected_epoch = 0u;
    for (uint8_t i = 0u; i < kSampleVoices; ++i) {
      if ((used & static_cast<uint8_t>(1u << i)) == 0u &&
          voices_[i].urgent_pending &&
          (best == 0xFFu || voices_[i].urgent_epoch > selected_epoch)) {
        best = i;
        selected_epoch = voices_[i].urgent_epoch;
      }
    }
    if (best == 0xFFu) {
      break;
    }
    dst[count++] = best;
    used |= static_cast<uint8_t>(1u << best);
  }
  return count;
}

bool SampleDryMixer::UrgentSofPending() const
{
  for (uint8_t i = 0u; i < kSampleVoices; ++i) {
    if (voices_[i].urgent_pending && voices_[i].sof_pending) {
      return true;
    }
  }
  return false;
}

void SampleDryMixer::EndUrgentPrefill(uint8_t active_mask)
{
  (void)active_mask;
  /* A vq can arrive after nX but before the complete bridge is queued. It
   * reconciles later refills; it must not truncate the ordered 15 ms runway. */
  DrainCmds();
}

bool SampleDryMixer::FillUrgentBurstForVoice(uint8_t voice, int16_t *dst,
                                              unsigned max_n,
                                              double quantum_ms,
                                              UrgentBurst &info)
{
  info = UrgentBurst{};
  DrainCmds();
  if (voice >= kSampleVoices || !voices_[voice].urgent_pending) {
    return false;
  }
  auto &v = voices_[voice];
  info.voice = voice;
  info.session = v.session;
  info.wave_id = v.wave_id;
  info.sof = v.sof_pending;
  if (!(quantum_ms > 0.0)) {
    return false;
  }
  const unsigned quantum = static_cast<unsigned>(std::ceil(
      IncOf(v) * static_cast<double>(kSampleRateHz) * quantum_ms / 1000.0));
  unsigned n = std::min({max_n, v.urgent_budget, quantum, kBodyBurstMax});
  if (n == 0u) {
    /* RS485 already started the note. Keep SOF pending and let the next vq
     * provide BODY once crash-release credit is available. */
    v.urgent_pending = false;
    urgent_ready_.fetch_sub(1u, std::memory_order_release);
    return false;
  }
  if (n != 0u && dst == nullptr) {
    return false;
  }
  for (unsigned i = 0u; i < n; ++i) {
    dst[i] = NextBody(v);
  }
  info.nsamp = n;
  v.queued += static_cast<double>(n);
  v.urgent_budget -= n;
  v.sof_pending = false;
  if (v.urgent_budget == 0u) {
    v.urgent_pending = false;
    urgent_ready_.fetch_sub(1u, std::memory_order_release);
  }
  sent_[voice].fetch_add(n, std::memory_order_release);
  return true;
}

double SampleDryMixer::VoiceSourceSamplesPerMs(uint8_t voice) const
{
  if (voice >= kSampleVoices || !voices_[voice].urgent_pending) {
    return 0.0;
  }
  return IncOf(voices_[voice]) *
         (static_cast<double>(kSampleRateHz) / 1000.0);
}

double SampleDryMixer::VoiceAttackLeadMs(uint8_t voice) const
{
  const double demand = VoiceSourceSamplesPerMs(voice);
  if (!(demand > 0.0)) {
    return 0.0;
  }
  return std::max(0.0, Fade0Of(voices_[voice].attack_len) / demand -
                           voices_[voice].attack_elapsed_ms);
}

void SampleDryMixer::AbortUrgentBurst(uint8_t voice, uint8_t session,
                                      uint16_t wave_id, unsigned nsamp,
                                      bool sof)
{
  if (!BurstIsCurrent(voice, session, wave_id)) {
    return;
  }
  AbortBurst(voice, session, wave_id, nsamp, sof);
  auto &v = voices_[voice];
  v.urgent_budget += nsamp;
  if (!v.urgent_pending) {
    v.urgent_pending = true;
    urgent_ready_.fetch_add(1u, std::memory_order_release);
  }
}

unsigned SampleDryMixer::QueuedSamples(uint8_t voice) const
{
  if (voice >= kSampleVoices) {
    return 0;
  }
  if (!live_[voice].load(std::memory_order_acquire)) {
    return 0;
  }
  return sent_[voice].load(std::memory_order_acquire);
}

bool SampleDryMixer::WaitPrefill(uint8_t voice, unsigned timeout_ms)
{
  const auto t0 = std::chrono::steady_clock::now();
  while (voice < kSampleVoices &&
         live_[voice].load(std::memory_order_acquire)) {
    if (committed_[voice].load(std::memory_order_acquire) >= kBodyBurstMax) {
      return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    if (elapsed.count() >= static_cast<long>(timeout_ms)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void SampleDryMixer::NoteOff(uint8_t voice)
{
  (void)voice;
}

void SampleDryMixer::Silence(uint8_t voice)
{
  if (voice >= kSampleVoices) {
    return;
  }
  Cmd c;
  c.kind = CmdKind::Silence;
  c.voice = voice;
  (void)Post(c);
}

void SampleDryMixer::AllNotesOff()
{
  Cmd c;
  c.kind = CmdKind::AllOff;
  (void)Post(c);
}

void SampleDryMixer::DrainPendingCommands()
{
  DrainCmds();
}

bool SampleDryMixer::AnyActive() const
{
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    if (live_[i].load(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

uint16_t SampleDryMixer::LiveWave(uint8_t voice) const
{
  if (voice >= kSampleVoices ||
      !live_[voice].load(std::memory_order_acquire)) {
    return 0xFFFFu;
  }
  return live_wave_[voice].load(std::memory_order_acquire);
}

int16_t SampleDryMixer::NextBody(Voice &v)
{
  auto &body = bodies_[v.wave_id];
  const size_t n = body.size();
  if (n == 0) {
    v.active = false;
    return 0;
  }
  const bool oneshot = oneshot_[v.wave_id].load(std::memory_order_relaxed);
  double ph = v.cursor;
  const double n_d = static_cast<double>(n);
  if (oneshot && ph >= n_d) {
    v.cursor = n_d;
    return 0;
  }
  if (!oneshot) {
    if (ph >= n_d || ph < 0.0) {
      ph = std::fmod(ph, n_d);
      if (ph < 0.0) {
        ph += n_d;
      }
    }
  } else if (ph < 0.0) {
    ph = 0.0;
  }
  const size_t i0 = std::min(static_cast<size_t>(ph), n - 1u);
  const int16_t s = body[i0];
  ph += 1.0;
  if (oneshot) {
    if (ph > n_d) {
      ph = n_d;
    }
  } else if (ph >= n_d) {
    ph = std::fmod(ph, n_d);
  }
  v.cursor = ph;
  return s;
}

void SampleDryMixer::ApplyVoiceStatus(uint8_t mask, uint8_t best,
                                      const uint16_t *free_samples,
                                      const uint16_t *unreflected)
{
  if (free_samples == nullptr) {
    return;
  }
  vq_mask_.store(mask, std::memory_order_relaxed);
  vq_best_.store(best, std::memory_order_relaxed);
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    const unsigned free_s = std::min(
        kRingSamples, static_cast<unsigned>(free_samples[i]));
    const uint16_t fill = static_cast<uint16_t>(kRingSamples - free_s);
    vq_fill_[i].store(fill, std::memory_order_relaxed);
    vq_fill_max_[i].store(fill, std::memory_order_relaxed);
    vq_free_[i].store(static_cast<uint16_t>(free_s),
                      std::memory_order_relaxed);
    vq_unreflected_[i].store(unreflected != nullptr ? unreflected[i] : 0u,
                             std::memory_order_relaxed);
  }
  vq_seq_.fetch_add(1, std::memory_order_release);
}

} // namespace audio
} // namespace cardlink
