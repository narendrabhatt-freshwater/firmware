#include "cardlink/audio/wave_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace cardlink {
namespace audio {
namespace {

constexpr int kLanczosA = 3;

bool HasExtI(const std::string &path, const char *ext)
{
  const size_t n = std::strlen(ext);
  if (path.size() < n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(path[path.size() - n + i]);
    const unsigned char b = static_cast<unsigned char>(ext[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

double I8ToDouble(int8_t value)
{
  return value < 0 ? static_cast<double>(value) / 128.0
                   : static_cast<double>(value) / 127.0;
}

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

int8_t FToI8(double y)
{
  y = std::clamp(y, -1.0, 1.0);
  const double s = y * 127.0;
  if (s >= 127.0) {
    return 127;
  }
  if (y <= -1.0) {
    return static_cast<int8_t>(-128);
  }
  return static_cast<int8_t>(std::lround(s));
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
      } else if (fmt == 1 && bits == 8) {
        acc += I8ToDouble(static_cast<int8_t>(static_cast<int>(s[0]) - 128));
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

bool ParseRawI8(const std::vector<uint8_t> &bytes, std::vector<double> &mono,
                std::string &err)
{
  if (bytes.empty()) {
    err = "raw must contain signed int8 samples";
    return false;
  }
  mono.resize(bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i) {
    const int8_t v = static_cast<int8_t>(bytes[i]);
    mono[i] = I8ToDouble(v);
  }
  return true;
}

void Split48k(const std::vector<double> &x, LoadedWave &out)
{
  const size_t n = x.size();
  const size_t alen = std::min(n, static_cast<size_t>(kAttackSamples));
  out.attack.resize(alen);
  for (size_t i = 0; i < alen; ++i) {
    out.attack[i] = FToI8(x[i]);
  }
  const size_t fade = std::min(static_cast<size_t>(kCrossfadeSamples), alen);
  const size_t b0 = alen - fade;
  out.body.resize(n > b0 ? (n - b0) : 0);
  for (size_t i = 0; i < out.body.size(); ++i) {
    if (i < fade) {
      out.body[i] = out.attack[b0 + i];
    } else {
      out.body[i] = FToI8(x[b0 + i]);
    }
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
  } else if (!ParseRawI8(bytes, mono, err)) {
    return false;
  }
  std::vector<double> x48;
  ResampleTo48k(mono, rate, x48);
  if (x48.empty()) {
    err = "empty wave";
    return false;
  }
  out.src_rate_hz = rate;
  Split48k(x48, out);
  return true;
}

bool BodyWithHeadOverlap(const std::string &head_i8_path,
                         const std::string &body_i8_path,
                         std::vector<int8_t> &body_out, std::string &err)
{
  if (!HasExtI(head_i8_path, ".i8") || !HasExtI(body_i8_path, ".i8")) {
    err = "split attack and BODY files must use .i8";
    return false;
  }
  std::vector<uint8_t> head;
  std::vector<uint8_t> body;
  if (!ReadAll(head_i8_path, head, err) || !ReadAll(body_i8_path, body, err)) {
    return false;
  }
  if (head.empty() || head.size() > kAttackSamples) {
    err = "head must be 1..512 signed int8 samples";
    return false;
  }
  const size_t head_n = head.size();
  if (body.empty()) {
    err = "body must contain signed int8 samples";
    return false;
  }
  const size_t fade = std::min(static_cast<size_t>(kCrossfadeSamples), head_n);
  body_out.clear();
  body_out.reserve(fade + body.size());
  const size_t start = head_n - fade;
  for (size_t i = 0; i < fade; ++i) {
    body_out.push_back(static_cast<int8_t>(head[start + i]));
  }
  for (uint8_t sample : body) {
    body_out.push_back(static_cast<int8_t>(sample));
  }
  return true;
}

} // namespace audio
} // namespace cardlink
