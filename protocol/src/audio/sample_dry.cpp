#include "cardlink/audio/sample_dry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

namespace cardlink {
namespace audio {

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
    live_[i].store(false, std::memory_order_relaxed);
    live_wave_[i].store(0xFFFFu, std::memory_order_relaxed);
    vq_fill_[i].store(0, std::memory_order_relaxed);
    vq_free_[i].store(static_cast<uint16_t>(kRingSamples),
                      std::memory_order_relaxed);
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
  const double root = root_hz_[v.wave_id].load(std::memory_order_relaxed);
  double inc = 1.0;
  if (root > 0.0 && v.freq_hz > 0.0) {
    inc = v.freq_hz / root;
  }
  if (inc > 16.0) {
    inc = 16.0;
  }
  if (inc < (1.0 / 16.0)) {
    inc = 1.0 / 16.0;
  }
  return inc;
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
    const double take = inc * nframes;
    v.queued -= take;
    if (v.queued < 0.0) {
      v.queued = 0.0;
    }
    v.fill -= take;
    if (v.fill < 0.0) {
      v.fill = 0.0;
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
    if (sent_[i].load(std::memory_order_relaxed) == 0u) {
      continue;
    }
    if ((mask & static_cast<uint8_t>(1u << i)) == 0u) {
      continue;
    }
    /* Ring occupancy is vq; fill is USB-accepted outstanding. Do not mix. */
    v.vq_free = vq_free_[i].load(std::memory_order_relaxed);
    v.fill_at_vq = v.fill;
    v.vq_have = true;
  }
}

double SampleDryMixer::RemainingMs(const Voice &v) const
{
  const double inc = IncOf(v);
  const double rate = static_cast<double>(kSampleRateHz) * inc;
  if (rate <= 0.0) {
    return 0.0;
  }
  return 1000.0 * v.fill / rate;
}

unsigned SampleDryMixer::TargetFill(const Voice &v) const
{
  const double inc = IncOf(v);
  double target = (static_cast<double>(kStopMs) / 1000.0) *
                  static_cast<double>(kSampleRateHz) * inc;
  const double cap = static_cast<double>(kRingSamples - kRingHeadroom);
  if (target > cap) {
    target = cap;
  }
  if (target < static_cast<double>(kMinBurst)) {
    target = static_cast<double>(kMinBurst);
  }
  return static_cast<unsigned>(target);
}

unsigned SampleDryMixer::VqRoom(const Voice &v) const
{
  if (!v.vq_have) {
    return kRingSamples - kRingHeadroom;
  }
  if (v.vq_free == 0u) {
    return 0;
  }
  double credit = static_cast<double>(v.vq_free);
  if (v.fill > v.fill_at_vq) {
    credit -= (v.fill - v.fill_at_vq);
  }
  if (credit < 1.0) {
    return 0;
  }
  return static_cast<unsigned>(credit);
}

unsigned SampleDryMixer::WantBurst(uint8_t voice) const
{
  if (voice >= kSampleVoices || !voices_[voice].active) {
    return 0;
  }
  const auto &v = voices_[voice];
  if (v.vq_have) {
    const double card_fill =
        static_cast<double>(vq_fill_[voice].load(std::memory_order_relaxed));
    const double rate = static_cast<double>(kSampleRateHz) * IncOf(v);
    if (rate > 0.0 &&
        (1000.0 * card_fill / rate) >= static_cast<double>(kStopMs)) {
      return 0;
    }
  }
  const unsigned vq_room = VqRoom(v);
  if (vq_room == 0u) {
    return 0;
  }
  const unsigned target = TargetFill(v);
  if (v.fill >= static_cast<double>(target)) {
    return 0;
  }
  double room = static_cast<double>(target) - v.fill;
  const double hard =
      static_cast<double>(kRingSamples - kRingHeadroom) - v.fill;
  if (hard < room) {
    room = hard;
  }
  if (static_cast<double>(vq_room) < room) {
    room = static_cast<double>(vq_room);
  }
  if (room < 1.0) {
    return 0;
  }
  unsigned n = static_cast<unsigned>(room);
  if (n > kBodyBurstMax) {
    n = kBodyBurstMax;
  }
  const double t_ms = RemainingMs(v);
  if (t_ms >= static_cast<double>(kNeedMs) && n < kMinBurst) {
    return 0;
  }
  return n;
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
    unsigned j = i;
    while (j > 0 && RemainingMs(voices_[tmp[j - 1u]]) > t) {
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
  v.fill += static_cast<double>(n);
  const unsigned sent = sent_[voice].load(std::memory_order_relaxed) + n;
  sent_[voice].store(sent, std::memory_order_release);
  if (!v.active) {
    live_[voice].store(false, std::memory_order_release);
    live_wave_[voice].store(0xFFFFu, std::memory_order_release);
  }
  return n;
}

void SampleDryMixer::AbortBurst(uint8_t voice, unsigned nsamp, bool sof)
{
  if (voice >= kSampleVoices || nsamp == 0) {
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
  if (v.fill > static_cast<double>(nsamp)) {
    v.fill -= static_cast<double>(nsamp);
  } else {
    v.fill = 0.0;
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
  sent_[voice].store(sent > nsamp ? sent - nsamp : 0u, std::memory_order_release);
}

void SampleDryMixer::Post(const Cmd &c)
{
  for (;;) {
    const uint32_t w = cmd_wr_.load(std::memory_order_relaxed);
    const uint32_t r = cmd_rd_.load(std::memory_order_acquire);
    if ((w - r) < kCmdCap) {
      cmds_[w % kCmdCap] = c;
      cmd_wr_.store(w + 1u, std::memory_order_release);
      return;
    }
    std::this_thread::yield();
  }
}

void SampleDryMixer::ApplyCmd(const Cmd &c)
{
  if (c.kind == CmdKind::AllOff) {
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      const uint8_t session = voices_[i].session;
      voices_[i] = Voice{};
      voices_[i].session = session;
      sent_[i].store(0, std::memory_order_release);
      live_[i].store(false, std::memory_order_release);
      live_wave_[i].store(0xFFFFu, std::memory_order_release);
    }
    vq_mask_.store(0, std::memory_order_relaxed);
    vq_best_.store(0xFF, std::memory_order_relaxed);
    return;
  }
  if (c.voice >= kSampleVoices) {
    return;
  }
  auto &v = voices_[c.voice];
  if (c.kind == CmdKind::Pitch) {
    if (v.active) {
      v.freq_hz = c.freq_hz;
    }
    return;
  }
  if (c.kind == CmdKind::Silence) {
    const uint8_t session = v.session;
    v = Voice{};
    v.session = session;
    sent_[c.voice].store(0, std::memory_order_release);
    live_[c.voice].store(false, std::memory_order_release);
    live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    return;
  }
  if (c.wave_id >= bodies_.size() || bodies_[c.wave_id].empty()) {
    live_[c.voice].store(false, std::memory_order_release);
    live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    return;
  }
  const uint8_t session = static_cast<uint8_t>((v.session + 1u) % kStreamSessionMod);
  v = Voice{};
  v.active = true;
  v.sof_pending = true;
  v.session = session;
  v.wave_id = c.wave_id;
  v.freq_hz = c.freq_hz;
  v.phase = 0.0;
  v.queued = 0.0;
  /* Join index is this wave's committed head, not the voice slot.
   * MIDI plays wave_id = key on whatever voice was allocated. */
  v.attack_len = attack_len_[c.wave_id].load(std::memory_order_acquire);
  v.vq_seen = vq_seq_.load(std::memory_order_acquire);
  sent_[c.voice].store(0, std::memory_order_release);
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

void SampleDryMixer::NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz)
{
  if (voice >= kSampleVoices || wave_id >= bodies_.size()) {
    return;
  }
  /* Mark in-use before the bulk thread drains the command so SetBody
   * cannot reallocate a table FillBurst is about to read. */
  live_wave_[voice].store(wave_id, std::memory_order_release);
  live_[voice].store(true, std::memory_order_release);
  Cmd c;
  c.kind = CmdKind::On;
  c.voice = voice;
  c.wave_id = wave_id;
  c.freq_hz = freq_hz;
  Post(c);
}

void SampleDryMixer::SetPitchHz(uint8_t voice, double freq_hz)
{
  if (voice >= kSampleVoices) {
    return;
  }
  Cmd c;
  c.kind = CmdKind::Pitch;
  c.voice = voice;
  c.freq_hz = freq_hz;
  Post(c);
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
  while (true) {
    if (voice < kSampleVoices && live_[voice].load(std::memory_order_acquire) &&
        sent_[voice].load(std::memory_order_acquire) >= kBodyBurstMax) {
      return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    if (elapsed.count() >= static_cast<long>(timeout_ms)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool SampleDryMixer::WaitPrefillActive(unsigned timeout_ms)
{
  const auto t0 = std::chrono::steady_clock::now();
  while (true) {
    bool any = false;
    bool ready = true;
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      if (!live_[i].load(std::memory_order_acquire)) {
        continue;
      }
      any = true;
      if (sent_[i].load(std::memory_order_acquire) < kBodyBurstMax) {
        ready = false;
      }
    }
    if (!any || ready) {
      return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    if (elapsed.count() >= static_cast<long>(timeout_ms)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
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
  Post(c);
}

void SampleDryMixer::AllNotesOff()
{
  Cmd c;
  c.kind = CmdKind::AllOff;
  Post(c);
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

void SampleDryMixer::ApplyVoiceQuery(uint8_t mask, uint8_t best,
                                     const uint8_t *free_slots)
{
  if (free_slots == nullptr) {
    return;
  }
  vq_mask_.store(mask, std::memory_order_relaxed);
  vq_best_.store(best, std::memory_order_relaxed);
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    uint8_t slots = free_slots[i];
    uint16_t fill = 0;
    uint16_t free_samp = static_cast<uint16_t>(kRingSamples);
    if (slots != kVqSlotEmpty) {
      if (slots > kVqSlotMax) {
        slots = kVqSlotMax;
      }
      const unsigned free_s = static_cast<unsigned>(slots) * kVqSlotSamples;
      free_samp = (free_s > kRingSamples)
                      ? static_cast<uint16_t>(kRingSamples)
                      : static_cast<uint16_t>(free_s);
      fill = (free_s >= kRingSamples)
                 ? static_cast<uint16_t>(0)
                 : static_cast<uint16_t>(kRingSamples - free_s);
    }
    vq_fill_[i].store(fill, std::memory_order_relaxed);
    vq_free_[i].store(free_samp, std::memory_order_relaxed);
  }
  vq_seq_.fetch_add(1, std::memory_order_release);
}

} // namespace audio
} // namespace cardlink
