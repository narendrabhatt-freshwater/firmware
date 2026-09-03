#include "cardlink/usb/attack_upload.hpp"
#include "cardlink/audio/sample_dry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace cardlink {
namespace usb {
namespace {

bool WaitForSubstring(cardlink::SerialPort &port,
                      const char *needle,
                      std::string &accum,
                      uint32_t timeout_ms,
                      bool fail_on_err)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint8_t buf[256];
  while (std::chrono::steady_clock::now() < deadline) {
    const size_t n = port.ReadTimeout(buf, sizeof(buf), 20);
    if (n > 0) {
      accum.append(reinterpret_cast<const char *>(buf), n);
      if (accum.find(needle) != std::string::npos) {
        return true;
      }
    }
    if (fail_on_err && accum.find("err:") != std::string::npos) {
      return false;
    }
  }
  return false;
}

std::string SanitizeRx(std::string s)
{
  for (char &c : s) {
    if (c != '\r' && c != '\n' && (c < 32 || c > 126)) {
      c = '.';
    }
  }
  if (s.size() > 120) {
    s.resize(120);
    s += "...";
  }
  return s;
}

void DrainRx(cardlink::SerialPort &port)
{
  uint8_t buf[256];
  for (int i = 0; i < 8; ++i) {
    if (port.ReadTimeout(buf, sizeof(buf), 5) == 0) {
      break;
    }
  }
}

void PollRx(cardlink::SerialPort &port, std::string &accum)
{
  uint8_t buf[256];
  for (;;) {
    const size_t n = port.ReadTimeout(buf, sizeof(buf), 0);
    if (n == 0) {
      break;
    }
    accum.append(reinterpret_cast<const char *>(buf), n);
  }
}

AttackUploadResult Fail(const std::string &msg)
{
  AttackUploadResult r;
  r.ok = false;
  r.message = msg;
  return r;
}

constexpr size_t kAttackBytes = cardlink::audio::kAttackSamples;

} // namespace

AttackUploader::AttackUploader(cardlink::SerialPort &cdc_port)
    : port_(cdc_port)
{
}

AttackUploadResult AttackUploader::Upload(
    uint16_t wave_id,
    const uint8_t *data,
    size_t nbytes,
    const std::function<void(float)> &on_progress)
{
  if (!port_.IsOpen()) {
    return Fail("err: CDC session not open");
  }
  if (wave_id >= cardlink::audio::kAttackWaves) {
    return Fail("err: wave_id 0..255");
  }
  if (data == nullptr || nbytes == 0u || nbytes > kAttackBytes) {
    return Fail("err: attack head must be 1..kAttackSamples signed int8");
  }

  DrainRx(port_);
  port_.FlushInput();

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:al %u %zu\r",
                static_cast<unsigned>(wave_id), nbytes);
  if (!port_.Write(reinterpret_cast<const uint8_t *>(cmd),
                   std::strlen(cmd))) {
    return Fail("err: CDC write al");
  }
  port_.DrainOutput();

  std::string rx;
  if (!WaitForSubstring(port_, "ok:ready", rx, 2000, true)) {
    return Fail(rx.empty() ? "err: no ok:ready"
                           : "err: " + SanitizeRx(rx));
  }

  if (on_progress) {
    on_progress(0.05f);
  }

  rx.clear();
  size_t off = 0;
  while (off < nbytes) {
    const size_t chunk = std::min<size_t>(512, nbytes - off);
    if (!port_.Write(data + off, chunk)) {
      return Fail("err: CDC write payload");
    }
    off += chunk;
    PollRx(port_, rx);
    if (rx.find("err:") != std::string::npos) {
      return Fail("err: card reply " + SanitizeRx(rx));
    }
    if (on_progress) {
      on_progress(0.05f + 0.9f * static_cast<float>(off) /
                              static_cast<float>(nbytes));
    }
  }
  port_.DrainOutput();

  if (rx.find("ok:attack") == std::string::npos) {
    if (!WaitForSubstring(port_, "ok:attack", rx, 3000, true)) {
      return Fail("err: no ok:attack (" + SanitizeRx(rx) + ")");
    }
  }

  if (on_progress) {
    on_progress(1.f);
  }

  AttackUploadResult out;
  out.ok = true;
  out.wave_id = wave_id;
  out.message = "ok:attack uploaded";
  return out;
}

AttackUploadResult AttackUploader::UploadFile(
    uint16_t wave_id,
    const std::string &file_path,
    const std::function<void(float)> &on_progress)
{
  if (file_path.size() < 3u || file_path[file_path.size() - 3u] != '.' ||
      std::tolower(static_cast<unsigned char>(file_path[file_path.size() - 2u])) != 'i' ||
      file_path.back() != '8') {
    return Fail("err: split attack must be signed .i8");
  }
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    return Fail("err: cannot open " + file_path);
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  if (data.empty()) {
    return Fail("err: empty signed int8 head");
  }
  if (data.size() > kAttackBytes) {
    return Fail("err: head longer than kAttackSamples");
  }
  return Upload(wave_id, data.data(), data.size(), on_progress);
}

} // namespace usb
} // namespace cardlink
