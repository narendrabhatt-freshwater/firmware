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
    vq_free_samp_[i].store(0, std::memory_order_relaxed);
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

void SampleDryMixer::AdvancePlayheads()
{
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    auto &v = voices_[i];
    if (!v.active) {
      continue;
    }
    const double inc = IncOf(v);
    v.phase += inc;
    if (v.phase >= Fade0Of(v.attack_len) && v.queued > 0.0) {
      const double take = (inc < v.queued) ? inc : v.queued;
      v.queued -= take;
    }
  }
}

bool SampleDryMixer::HasRoom(const Voice &v)
{
  /* After the start is in the FIFO, wait until the card has played
   * that many source samples past fade0. Sending sooner is dropped
   * and the cursor skips (gap after the AXI head). */
  if (v.attack_len > 0u &&
      v.cursor >= static_cast<double>(kRingSamples) &&
      v.phase < Fade0Of(v.attack_len) + static_cast<double>(kRingSamples)) {
    return false;
  }
  return v.queued + static_cast<double>(kUacBodyPerFrame) <=
         static_cast<double>(kRingSamples);
}

int16_t SampleDryMixer::EncodeTag(uint8_t session, uint8_t route_low)
{
  const uint8_t low = static_cast<uint8_t>((session << kUacSessionShift) | route_low);
  return static_cast<int16_t>(kUacTagBase | low);
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
    vq_live_.store(false, std::memory_order_release);
    vq_mask_.store(0, std::memory_order_relaxed);
    vq_best_.store(0xFF, std::memory_order_relaxed);
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      vq_free_samp_[i].store(0, std::memory_order_relaxed);
      vq_reclaim_[i] = 0.0;
      vq_feed_[i].store(static_cast<uint8_t>(VqFeed::Prefill),
                        std::memory_order_relaxed);
    }
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
    vq_free_samp_[c.voice].store(0, std::memory_order_relaxed);
    vq_reclaim_[c.voice] = 0.0;
    vq_feed_[c.voice].store(static_cast<uint8_t>(VqFeed::Prefill),
                            std::memory_order_relaxed);
    return;
  }
  if (c.wave_id >= bodies_.size() || bodies_[c.wave_id].empty()) {
    live_[c.voice].store(false, std::memory_order_release);
    live_wave_[c.voice].store(0xFFFFu, std::memory_order_release);
    return;
  }
  const uint8_t session = static_cast<uint8_t>((v.session + 1u) % kUacSessionMod);
  v = Voice{};
  v.active = true;
  v.sof_pending = true;
  v.session = session;
  v.wave_id = c.wave_id;
  v.freq_hz = c.freq_hz;
  v.phase = 0.0;
  v.queued = 0.0;
  /* Head length is per card slot (AXI id = voice), not the body table.
   * MIDI may stream another slot's body onto an empty head. */
  v.attack_len = attack_len_[c.voice].load(std::memory_order_acquire);
  sent_[c.voice].store(0, std::memory_order_release);
  live_wave_[c.voice].store(c.wave_id, std::memory_order_release);
  live_[c.voice].store(true, std::memory_order_release);
  vq_free_samp_[c.voice].store(0, std::memory_order_relaxed);
  vq_reclaim_[c.voice] = 0.0;
  vq_feed_[c.voice].store(static_cast<uint8_t>(VqFeed::Prefill),
                          std::memory_order_relaxed);
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
  /* Mark in-use before the callback drains the command so SetBody
   * cannot reallocate a table Render is about to read. */
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
        sent_[voice].load(std::memory_order_acquire) >= kPrefillSamples) {
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
      if (sent_[i].load(std::memory_order_acquire) < kPrefillSamples) {
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

double SampleDryMixer::FilledEstimate(uint8_t voice) const
{
  if (voice >= kSampleVoices) {
    return 0.0;
  }
  const auto feed = static_cast<VqFeed>(
      vq_feed_[voice].load(std::memory_order_relaxed));
  const bool have_vq = vq_live_.load(std::memory_order_relaxed);
  const uint8_t mask = vq_mask_.load(std::memory_order_relaxed);
  /* Prefill / not-yet-on-card: vq_free_samp_ is 0, which would look
   * like a full ring and starve the new voice — or, if we exclusive-
   * feed it, starve the note already in Drain. Use bytes actually
   * pushed until the card has listed this slot. */
  if (!have_vq || feed == VqFeed::Prefill ||
      (mask & static_cast<uint8_t>(1u << voice)) == 0) {
    return static_cast<double>(
        sent_[voice].load(std::memory_order_relaxed));
  }
  const uint16_t free = vq_free_samp_[voice].load(std::memory_order_relaxed);
  if (free >= kRingSamples) {
    return 0.0;
  }
  return static_cast<double>(kRingSamples - free);
}

uint8_t SampleDryMixer::PickVoice() const
{
  /* Drain (and catch-up prefill) first: a held note with credit must
   * not lose the frame to a new key whose sent count is still 0.
   * On-schedule prefill takes leftover frames. */
  uint8_t pick = 0xFF;
  double least_t = 0.0;
  for (int pass = 0; pass < 2; ++pass) {
    pick = 0xFF;
    least_t = 0.0;
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      if (!CanSend(i)) {
        continue;
      }
      const auto feed = static_cast<VqFeed>(
          vq_feed_[i].load(std::memory_order_relaxed));
      const bool urgent = (feed != VqFeed::Prefill) || PrefillCatchUp(i);
      if (pass == 0 && !urgent) {
        continue;
      }
      if (pass == 1 && urgent) {
        continue;
      }
      const double t = FilledEstimate(i) / IncOf(voices_[i]);
      if (pick == 0xFF || t < least_t) {
        least_t = t;
        pick = i;
      }
    }
    if (pick != 0xFF) {
      return pick;
    }
  }
  return 0xFF;
}

bool SampleDryMixer::PrefillCatchUp(uint8_t voice) const
{
  if (voice >= kSampleVoices) {
    return false;
  }
  const auto &v = voices_[voice];
  if (v.attack_len == 0u) {
    return true;
  }
  const double fade0 = Fade0Of(v.attack_len);
  if (v.phase >= fade0) {
    return true;
  }
  const unsigned n = sent_[voice].load(std::memory_order_relaxed);
  /* phase tracks source samples of the head (+= inc per UAC frame).
   * On schedule, sent ≈ phase. More than one packet late → dump. */
  return static_cast<double>(n + kUacBodyPerFrame) < v.phase;
}

bool SampleDryMixer::CanSend(uint8_t voice) const
{
  if (voice >= kSampleVoices) {
    return false;
  }
  const auto &v = voices_[voice];
  if (!v.active) {
    return false;
  }
  if (vq_live_.load(std::memory_order_relaxed)) {
    const auto feed = static_cast<VqFeed>(
        vq_feed_[voice].load(std::memory_order_relaxed));
    if (feed == VqFeed::Full) {
      return false;
    }
    const unsigned burst =
        vq_free_samp_[voice].load(std::memory_order_relaxed);
    const unsigned trickle = static_cast<unsigned>(vq_reclaim_[voice]);
    if (feed == VqFeed::Prefill) {
      const unsigned n = sent_[voice].load(std::memory_order_relaxed);
      if (n >= kPrefillSamples && burst < kUacBodyPerFrame) {
        return false;
      }
      if (PrefillCatchUp(voice)) {
        return burst >= kUacBodyPerFrame || n < kPrefillSamples;
      }
      /* Same cadence as Drain: ~inc body samples per output sample so
       * the ring fills over the attack instead of hogging the pipe. */
      return (burst + trickle) >= kUacBodyPerFrame;
    }
    return (burst + trickle) >= kUacBodyPerFrame;
  }
  return HasRoom(v);
}

void SampleDryMixer::SpendVq(uint8_t voice)
{
  if (!vq_live_.load(std::memory_order_relaxed) || voice >= kSampleVoices) {
    return;
  }
  const uint16_t burst = vq_free_samp_[voice].load(std::memory_order_relaxed);
  if (burst >= kUacBodyPerFrame) {
    vq_free_samp_[voice].store(
        static_cast<uint16_t>(burst - kUacBodyPerFrame),
        std::memory_order_relaxed);
    return;
  }
  vq_free_samp_[voice].store(0, std::memory_order_relaxed);
  const double need = static_cast<double>(kUacBodyPerFrame - burst);
  if (vq_reclaim_[voice] > need) {
    vq_reclaim_[voice] -= need;
  } else {
    vq_reclaim_[voice] = 0.0;
  }
}

void SampleDryMixer::ReclaimVq()
{
  if (!vq_live_.load(std::memory_order_relaxed)) {
    return;
  }
  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    const auto &v = voices_[i];
    if (!v.active) {
      vq_reclaim_[i] = 0.0;
      continue;
    }
    const auto feed = static_cast<VqFeed>(
        vq_feed_[i].load(std::memory_order_relaxed));
    if (feed == VqFeed::Full) {
      continue;
    }
    vq_reclaim_[i] += IncOf(v);
    if (vq_reclaim_[i] > static_cast<double>(kRingSamples)) {
      vq_reclaim_[i] = static_cast<double>(kRingSamples);
    }
  }
}

void SampleDryMixer::Render(int16_t *interleaved, unsigned nframes)
{
  if (!interleaved || nframes == 0) {
    return;
  }
  DrainCmds();
  for (unsigned f = 0; f < nframes; ++f) {
    int16_t *fr = &interleaved[f * kUacChannels];
    AdvancePlayheads();
    ReclaimVq();
    const uint8_t voice = PickVoice();
    if (voice == 0xFF) {
      fr[0] = static_cast<int16_t>(kUacTagBase | kUacIdle);
      for (unsigned ch = 1; ch < kUacChannels; ++ch) {
        fr[ch] = 0;
      }
      continue;
    }
    auto &v = voices_[voice];
    uint8_t route = voice;
    if (v.sof_pending) {
      route = static_cast<uint8_t>(voice | kUacSof);
      v.sof_pending = false;
    }
    fr[0] = EncodeTag(v.session, route);
    for (unsigned ch = 1; ch < kUacChannels; ++ch) {
      fr[ch] = NextBody(v);
    }
    v.queued += static_cast<double>(kUacBodyPerFrame);
    const unsigned n = sent_[voice].load(std::memory_order_relaxed) + kUacBodyPerFrame;
    sent_[voice].store(n, std::memory_order_release);
    SpendVq(voice);
    if (!v.active) {
      live_[voice].store(false, std::memory_order_release);
      live_wave_[voice].store(0xFFFFu, std::memory_order_release);
    }
  }
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
    if (slots > kVqSlotMax) {
      slots = kVqSlotMax;
    }
    const uint16_t samples = (slots == kVqSlotMax)
                                 ? static_cast<uint16_t>(kRingSamples)
                                 : static_cast<uint16_t>(slots * kVqSlotSamples);
    vq_free_samp_[i].store(samples, std::memory_order_relaxed);
    const uint8_t st = vq_feed_[i].load(std::memory_order_relaxed);
    if (slots == 0u) {
      if (st == static_cast<uint8_t>(VqFeed::Prefill)) {
        vq_feed_[i].store(static_cast<uint8_t>(VqFeed::Full),
                          std::memory_order_relaxed);
      }
    } else if (st == static_cast<uint8_t>(VqFeed::Full)) {
      /* Hole after a full ring: card is consuming body. */
      vq_feed_[i].store(static_cast<uint8_t>(VqFeed::Drain),
                        std::memory_order_relaxed);
    }
  }
  vq_live_.store(true, std::memory_order_release);
}

} // namespace audio
} // namespace cardlink
