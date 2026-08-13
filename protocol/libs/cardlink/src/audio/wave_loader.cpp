#include "cardlink/audio/wave_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace cardlink {
namespace audio {
namespace {

constexpr int kLanczosA = 3;

double Sinc(double x)
{
  if (std::fabs(x) < 1e-12) {
    return 1.0;
  }
  const double px = x * 3.14159265358979323846;
  return std::sin(px) / px;
}

double Lanczos(double x)
{
  if (x <= -kLanczosA || x >= kLanczosA) {
    return 0.0;
  }
  return Sinc(x) * Sinc(x / static_cast<double>(kLanczosA));
}

int16_t FToI16(double y)
{
  y = std::clamp(y, -1.0, 1.0);
  return static_cast<int16_t>(std::lround(y * 32767.0));
}

int32_t FToQ31(double y)
{
  y = std::clamp(y, -1.0, 1.0);
  const double s = y * 2147483647.0;
  if (s >= 2147483647.0) {
    return 0x7FFFFFFF;
  }
  if (s < -2147483648.0) {
    return static_cast<int32_t>(0x80000000);
  }
  return static_cast<int32_t>(std::lround(s));
}

void ResampleTo48k(const std::vector<double> &in, uint32_t in_rate,
                   std::vector<double> &out)
{
  if (in.empty() || in_rate == 0) {
    out.clear();
    return;
  }
  if (in_rate == kSampleRateHz) {
    out = in;
    return;
  }
  const double ratio = static_cast<double>(in_rate) / kSampleRateHz;
  const size_t nout = static_cast<size_t>(
      std::ceil(static_cast<double>(in.size()) / ratio));
  out.resize(nout);
  const int n = static_cast<int>(in.size());
  for (size_t j = 0; j < nout; ++j) {
    const double src = static_cast<double>(j) * ratio;
    const int i0 = static_cast<int>(std::floor(src));
    double acc = 0.0;
    double wsum = 0.0;
    for (int t = -kLanczosA + 1; t <= kLanczosA; ++t) {
      const int i = i0 + t;
      if (i < 0 || i >= n) {
        continue;
      }
      const double w = Lanczos(src - static_cast<double>(i));
      acc += in[static_cast<size_t>(i)] * w;
      wsum += w;
    }
    out[j] = (wsum > 1e-12) ? (acc / wsum) : 0.0;
  }
}

bool ReadAll(const std::string &path, std::vector<uint8_t> &bytes,
             std::string &err)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open " + path;
    return false;
  }
  bytes.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
  return true;
}

uint16_t U16(const uint8_t *p)
{
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t U32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool ParseWav(const std::vector<uint8_t> &bytes, std::vector<double> &mono,
              uint32_t &rate, std::string &err)
{
  if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    err = "not a RIFF WAVE";
    return false;
  }
  size_t off = 12;
  uint16_t fmt = 1, ch = 1, bits = 16;
  rate = kSampleRateHz;
  const uint8_t *data = nullptr;
  uint32_t data_len = 0;
  while (off + 8 <= bytes.size()) {
    const uint32_t sz = U32(bytes.data() + off + 4);
    const uint8_t *payload = bytes.data() + off + 8;
    if (std::memcmp(bytes.data() + off, "fmt ", 4) == 0 && sz >= 16) {
      fmt = U16(payload);
      ch = U16(payload + 2);
      rate = U32(payload + 4);
      bits = U16(payload + 14);
    } else if (std::memcmp(bytes.data() + off, "data", 4) == 0) {
      data = payload;
      data_len = sz;
      break;
    }
    off += 8u + static_cast<size_t>((sz + 1u) & ~1u);
  }
  if (data == nullptr || ch == 0 || rate == 0) {
    err = "wav missing fmt/data";
    return false;
  }
  if (fmt != 1 && fmt != 3) {
    err = "wav format must be PCM or float";
    return false;
  }
  const uint32_t bpf = (static_cast<uint32_t>(bits) / 8u) * ch;
  if (bpf == 0) {
    err = "bad wav bits";
    return false;
  }
  const uint32_t frames = data_len / bpf;
  mono.resize(frames);
  for (uint32_t f = 0; f < frames; ++f) {
    const uint8_t *fr = data + f * bpf;
    double acc = 0.0;
    for (uint16_t c = 0; c < ch; ++c) {
      const uint8_t *s = fr + c * (bits / 8);
      if (fmt == 3 && bits == 32) {
        float fv = 0.f;
        std::memcpy(&fv, s, 4);
        acc += static_cast<double>(fv);
      } else if (bits == 16) {
        const int16_t v = static_cast<int16_t>(U16(s));
        acc += static_cast<double>(v) / 32768.0;
      } else if (bits == 24) {
        int32_t v = static_cast<int32_t>(s[0] | (s[1] << 8) | (s[2] << 16));
        if (v & 0x800000) {
          v |= static_cast<int32_t>(0xFF000000);
        }
        acc += static_cast<double>(v) / 8388608.0;
      } else if (bits == 32 && fmt == 1) {
        int32_t v = 0;
        std::memcpy(&v, s, 4);
        acc += static_cast<double>(v) / 2147483648.0;
      } else {
        err = "unsupported wav bit depth";
        return false;
      }
    }
    mono[f] = acc / static_cast<double>(ch);
  }
  return true;
}

bool ParseRawI16(const std::vector<uint8_t> &bytes, std::vector<double> &mono,
                 std::string &err)
{
  if (bytes.size() < 2 || (bytes.size() & 1u) != 0u) {
    err = "raw must be even int16 LE";
    return false;
  }
  const size_t n = bytes.size() / 2;
  mono.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const int16_t v = static_cast<int16_t>(
        bytes[i * 2] | (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8));
    mono[i] = static_cast<double>(v) / 32768.0;
  }
  return true;
}

void Split48k(const std::vector<double> &x, LoadedWave &out)
{
  out.attack.assign(kAttackSamples, 0);
  const size_t n = x.size();
  const size_t alen = std::min(n, static_cast<size_t>(kAttackSamples));
  for (size_t i = 0; i < alen; ++i) {
    out.attack[i] = FToQ31(x[i]);
  }
  const size_t b0 = std::min(n, static_cast<size_t>(kBodyOrigin));
  out.body.resize(n > b0 ? (n - b0) : 0);
  for (size_t i = b0; i < n; ++i) {
    out.body[i - b0] = FToI16(x[i]);
  }
}

} // namespace

bool LoadWaveFile(const std::string &path, uint32_t raw_rate_hz,
                  LoadedWave &out, std::string &err)
{
  std::vector<uint8_t> bytes;
  if (!ReadAll(path, bytes, err)) {
    return false;
  }
  std::vector<double> mono;
  uint32_t rate = raw_rate_hz ? raw_rate_hz : kSampleRateHz;
  const bool wav = path.size() >= 4 &&
                   (path.compare(path.size() - 4, 4, ".wav") == 0 ||
                    path.compare(path.size() - 4, 4, ".WAV") == 0);
  if (wav) {
    if (!ParseWav(bytes, mono, rate, err)) {
      return false;
    }
  } else if (!ParseRawI16(bytes, mono, err)) {
    return false;
  }
  std::vector<double> x48;
  ResampleTo48k(mono, rate, x48);
  if (x48.size() < kAttackSamples) {
    err = "wave shorter than attack head";
    return false;
  }
  out.src_rate_hz = rate;
  Split48k(x48, out);
  return true;
}

bool BodyWithHeadOverlap(const std::string &head_i32_path,
                         const std::string &body_i16_path,
                         std::vector<int16_t> &body_out, std::string &err)
{
  std::vector<uint8_t> head;
  std::vector<uint8_t> body;
  if (!ReadAll(head_i32_path, head, err) || !ReadAll(body_i16_path, body, err)) {
    return false;
  }
  if (head.size() != kAttackSamples * 4u) {
    err = "head must be 1024 bytes";
    return false;
  }
  if (body.size() < 2 || (body.size() & 1u) != 0u) {
    err = "body must be even int16 LE";
    return false;
  }
  body_out.clear();
  body_out.reserve(kCrossfadeSamples + body.size() / 2);
  const size_t start = (kAttackSamples - kCrossfadeSamples) * 4u;
  for (unsigned i = 0; i < kCrossfadeSamples; ++i) {
    int32_t q = 0;
    std::memcpy(&q, head.data() + start + i * 4u, 4);
    body_out.push_back(static_cast<int16_t>(q >> 16));
  }
  for (size_t i = 0; i < body.size() / 2; ++i) {
    body_out.push_back(static_cast<int16_t>(
        body[i * 2] | (static_cast<uint16_t>(body[i * 2 + 1]) << 8)));
  }
  return true;
}

} // namespace audio
} // namespace cardlink
