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
                                  const std::string &path,
                                  std::string &err)
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
  std::lock_guard<std::mutex> lock(mu_);
  auto &dst = bodies_[wave_id];
  dst.resize(raw.size() / 2);
  for (size_t i = 0; i < dst.size(); ++i) {
    dst[i] = static_cast<int16_t>(
        static_cast<uint8_t>(raw[i * 2]) |
        (static_cast<uint16_t>(static_cast<uint8_t>(raw[i * 2 + 1])) << 8));
  }
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
    /* Optional third field: loop (default) | oneshot */
    if (iss >> mode) {
      bool one = (mode == "oneshot" || mode == "one-shot" || mode == "1");
      if (id < oneshot_.size()) {
        SetBodyOneshot(static_cast<uint16_t>(id), one);
      }
    }
  }
  return true;
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
  const double root = root_hz_[wave_id];
  double inc = 1.0;
  if (root > 0.0 && std::isfinite(root) && freq_hz > 0.0 &&
      std::isfinite(freq_hz)) {
    inc = freq_hz / root;
  }
  /* Match card attack pitch clamp. */
  if (inc > 16.0) {
    inc = 16.0;
  }
  if (inc < (1.0 / 16.0)) {
    inc = 1.0 / 16.0;
  }
  voices_[voice].active = true;
  voices_[voice].wave_id = wave_id;
  voices_[voice].phase = 0.0;
  voices_[voice].phase_inc = inc;
}

void SampleDryMixer::NoteOff(uint8_t voice)
{
  (void)voice;
  /* Keep looping through card release — Silence() when vq reports idle. */
}

void SampleDryMixer::Silence(uint8_t voice)
{
  if (voice >= kSampleVoices) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  voices_[voice].active = false;
  voices_[voice].phase = 0.0;
  voices_[voice].phase_inc = 1.0;
}

void SampleDryMixer::AllNotesOff()
{
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &v : voices_) {
    v.active = false;
    v.phase = 0.0;
    v.phase_inc = 1.0;
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

void SampleDryMixer::Render(int16_t *interleaved, unsigned nframes)
{
  if (!interleaved || nframes == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (unsigned f = 0; f < nframes; ++f) {
    for (unsigned ch = 0; ch < kSampleVoices; ++ch) {
      int16_t s = 0;
      auto &v = voices_[ch];
      if (v.active) {
        const auto &body = bodies_[v.wave_id];
        const size_t n = body.size();
        if (n == 0) {
          v.active = false;
        } else if (n == 1) {
          s = body[0];
          v.phase += v.phase_inc;
        } else {
          const bool oneshot = oneshot_[v.wave_id];
          double ph = v.phase;
          const double n_d = static_cast<double>(n);
          if (oneshot && ph >= n_d) {
            /* Past end: silence; keep streaming zeros until Silence(). */
            s = 0;
            v.phase = n_d;
          } else {
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
            const size_t i0 = static_cast<size_t>(ph);
            size_t i1 = i0 + 1u;
            double b_s;
            if (i1 >= n) {
              if (oneshot) {
                b_s = 0.0;
              } else {
                i1 = 0u;
                b_s = static_cast<double>(body[i1]);
              }
            } else {
              b_s = static_cast<double>(body[i1]);
            }
            const double frac = ph - static_cast<double>(i0);
            const double a_s = static_cast<double>(body[std::min(i0, n - 1u)]);
            const double y = a_s + (b_s - a_s) * frac;
            s = static_cast<int16_t>(
                std::lround(std::clamp(y, -32768.0, 32767.0)));
            ph += v.phase_inc;
            if (oneshot) {
              if (ph > n_d) {
                ph = n_d;
              }
            } else if (ph >= n_d) {
              ph = std::fmod(ph, n_d);
            }
            v.phase = ph;
          }
        }
      }
      interleaved[f * kSampleVoices + ch] = s;
    }
  }
}

} // namespace audio
} // namespace cardlink
