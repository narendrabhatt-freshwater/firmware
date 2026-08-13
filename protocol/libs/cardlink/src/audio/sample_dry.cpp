#include "cardlink/audio/sample_dry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace cardlink {
namespace audio {

SampleDryMixer::SampleDryMixer()
{
  root_hz_.fill(kDefaultBodyRootHz);
  oneshot_.fill(false);
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
  std::lock_guard<std::mutex> lock(mu_);
  bodies_[wave_id].assign(data, data + nsamp);
  return true;
}

void SampleDryMixer::SetBodyRootHz(uint16_t wave_id, double root_hz)
{
  if (wave_id >= root_hz_.size() || !(root_hz > 0.0) || !std::isfinite(root_hz)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  root_hz_[wave_id] = root_hz;
}

double SampleDryMixer::BodyRootHz(uint16_t wave_id) const
{
  if (wave_id >= root_hz_.size()) {
    return kDefaultBodyRootHz;
  }
  std::lock_guard<std::mutex> lock(mu_);
  return root_hz_[wave_id];
}

void SampleDryMixer::SetBodyOneshot(uint16_t wave_id, bool oneshot)
{
  if (wave_id >= oneshot_.size()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  oneshot_[wave_id] = oneshot;
}

bool SampleDryMixer::BodyOneshot(uint16_t wave_id) const
{
  if (wave_id >= oneshot_.size()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  return oneshot_[wave_id];
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

double SampleDryMixer::IncLocked(const Voice &v) const
{
  const double root = root_hz_[v.wave_id];
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

void SampleDryMixer::NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz)
{
  if (voice >= kSampleVoices || wave_id >= bodies_.size()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (bodies_[wave_id].empty()) {
    return;
  }
  auto &v = voices_[voice];
  v.active = true;
  v.sof_pending = true;
  v.wave_id = wave_id;
  v.freq_hz = freq_hz;
  v.cursor = 0.0;
  v.queued = 0.0;
  v.consumed = 0.0;
}

void SampleDryMixer::SetPitchHz(uint8_t voice, double freq_hz)
{
  if (voice >= kSampleVoices) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (voices_[voice].active) {
    voices_[voice].freq_hz = freq_hz;
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
  std::lock_guard<std::mutex> lock(mu_);
  voices_[voice] = Voice{};
}

void SampleDryMixer::AllNotesOff()
{
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &v : voices_) {
    v = Voice{};
  }
}

bool SampleDryMixer::AnyActive() const
{
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto &v : voices_) {
    if (v.active) {
      return true;
    }
  }
  return false;
}

int16_t SampleDryMixer::NextBodyLocked(Voice &v)
{
  auto &body = bodies_[v.wave_id];
  const size_t n = body.size();
  if (n == 0) {
    v.active = false;
    return 0;
  }
  const bool oneshot = oneshot_[v.wave_id];
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

uint8_t SampleDryMixer::PickVoiceLocked()
{
  uint8_t best = 0xFF;
  double best_rem = 1e300;
  uint8_t npre = 0;
  uint8_t pre[kSampleVoices];

  for (uint8_t i = 0; i < kSampleVoices; ++i) {
    auto &v = voices_[i];
    if (!v.active) {
      continue;
    }
    const double fill = v.queued - v.consumed;
    const double room = static_cast<double>(kRingSamples) - fill;
    if (room < 8.0) {
      continue;
    }
    if (v.queued < static_cast<double>(kPrefillSamples)) {
      pre[npre++] = i;
    }
    const double rem = v.queued - v.consumed;
    if (rem < best_rem) {
      best_rem = rem;
      best = i;
    }
  }
  if (npre != 0) {
    /* Prefill contract: new notes before hunger mux. */
    rr_ = static_cast<uint8_t>((rr_ + 1u) % npre);
    return pre[rr_ % npre];
  }
  if (best != 0xFF) {
    return best;
  }
  return 0xFF;
}

void SampleDryMixer::Render(int16_t *interleaved, unsigned nframes)
{
  if (!interleaved || nframes == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (unsigned f = 0; f < nframes; ++f) {
    int16_t *fr = &interleaved[f * kSampleVoices];
    /* Card consumes one body sample per output sample once in the body
     * region. Tick every voice every frame or the mux latches "full"
     * and never sends again after the first fill. */
    for (uint8_t i = 0; i < kSampleVoices; ++i) {
      auto &v = voices_[i];
      if (!v.active) {
        continue;
      }
      v.consumed += IncLocked(v);
      double fill = v.queued - v.consumed;
      if (fill < 0.0) {
        v.consumed = v.queued;
      } else if (fill > static_cast<double>(kRingSamples)) {
        v.queued = v.consumed + static_cast<double>(kRingSamples);
      }
    }
    const uint8_t voice = PickVoiceLocked();
    if (voice == 0xFF) {
      fr[0] = 0;
      for (unsigned ch = 1; ch < kSampleVoices; ++ch) {
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
    fr[0] = static_cast<int16_t>(kUacTagBase | route);
    for (unsigned ch = 1; ch < kSampleVoices; ++ch) {
      fr[ch] = NextBodyLocked(v);
    }
    v.queued += static_cast<double>(kSampleVoices - 1u);
  }
}

} // namespace audio
} // namespace cardlink
