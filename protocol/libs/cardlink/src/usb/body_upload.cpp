#include "cardlink/usb/body_upload.hpp"

#include <algorithm>
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

BodyUploadResult Fail(const std::string &msg)
{
  BodyUploadResult r;
  r.ok = false;
  r.message = msg;
  return r;
}

constexpr size_t kBodyBytesMax = 9600 * 2;

} // namespace

BodyUploader::BodyUploader(cardlink::SerialPort &cdc_port) : port_(cdc_port) {}

BodyUploadResult BodyUploader::Upload(
    uint16_t wave_id,
    const uint8_t *data,
    size_t nbytes,
    const std::function<void(float)> &on_progress)
{
  if (!port_.IsOpen()) {
    return Fail("err: CDC session not open");
  }
  if (wave_id >= 8u) {
    return Fail("err: wave_id 0..7");
  }
  if (data == nullptr || nbytes < 2 || (nbytes & 1u) != 0u ||
      nbytes > kBodyBytesMax) {
    return Fail("err: body must be even int16 LE, <= 19200 bytes");
  }

  DrainRx(port_);
  port_.FlushInput();

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:bl %u %zu\r",
                static_cast<unsigned>(wave_id), nbytes);
  if (!port_.Write(reinterpret_cast<const uint8_t *>(cmd),
                   std::strlen(cmd))) {
    return Fail("err: CDC write bl");
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

  if (rx.find("ok:body") == std::string::npos) {
    if (!WaitForSubstring(port_, "ok:body", rx, 5000, true)) {
      return Fail("err: no ok:body (" + SanitizeRx(rx) + ")");
    }
  }

  if (on_progress) {
    on_progress(1.f);
  }

  BodyUploadResult out;
  out.ok = true;
  out.wave_id = wave_id;
  out.message = "ok:body uploaded";
  return out;
}

BodyUploadResult BodyUploader::UploadFile(
    uint16_t wave_id,
    const std::string &file_path,
    const std::function<void(float)> &on_progress)
{
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    return Fail("err: cannot open " + file_path);
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  return Upload(wave_id, data.data(), data.size(), on_progress);
}

} // namespace usb
} // namespace cardlink
