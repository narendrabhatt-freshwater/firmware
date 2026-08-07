#include "host_io/usb/wave_upload.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

namespace host_io {
namespace usb {
namespace {

bool WaitForSubstring(host_io::SerialPort &port,
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
      if (fail_on_err && accum.find("err:") != std::string::npos) {
        return false;
      }
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

void DrainRx(host_io::SerialPort &port)
{
  uint8_t buf[256];
  for (int i = 0; i < 8; ++i) {
    if (port.ReadTimeout(buf, sizeof(buf), 5) == 0) {
      break;
    }
  }
}

void PollRx(host_io::SerialPort &port, std::string &accum)
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

WaveUploadResult Fail(const std::string &msg)
{
  WaveUploadResult r;
  r.ok = false;
  r.message = msg;
  return r;
}

} // namespace

bool LooksLikeRs485AdapterPath(const std::string &path)
{
  return path.find("usbserial") != std::string::npos ||
         path.find("SLAB_USBtoUART") != std::string::npos ||
         path.find("usb-serial") != std::string::npos ||
         path.find("ttyUSB") != std::string::npos;
}

WaveUploader::WaveUploader(host_io::SerialPort &cdc_port) : port_(cdc_port) {}

bool WaveUploader::OpenCdcPort(host_io::SerialPort &port,
                               const std::string &cdc_path,
                               std::string &err_out,
                               uint32_t baud)
{
  port.Close();
  if (cdc_path.empty()) {
    err_out = "err: no USB CDC path (Channel Card cu.usbmodem…)";
    return false;
  }
  if (LooksLikeRs485AdapterPath(cdc_path)) {
    err_out = "err: that looks like the RS485 adapter — pick Channel "
              "cu.usbmodem* / ttyACM* for wave upload";
    return false;
  }
  if (!port.Open(cdc_path, baud)) {
    err_out = "err: CDC open " + port.LastError();
    return false;
  }
  port.SetDtr(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  DrainRx(port);
  port.FlushInput();
  return true;
}

WaveUploadResult WaveUploader::Upload(
    uint8_t slot,
    const uint8_t *data,
    size_t nbytes,
    const std::function<void(float)> &on_progress)
{
  if (!port_.IsOpen()) {
    return Fail("err: CDC session not open");
  }
  if (slot > 7u) {
    return Fail("err: wave slot 0..7");
  }
  if (data == nullptr || nbytes < 2u || (nbytes & 1u) != 0u ||
      nbytes > 32768u) {
    return Fail("err: raw size must be even, 2..32768 bytes");
  }

  DrainRx(port_);
  port_.FlushInput();

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:wl %u %zu\r",
                static_cast<unsigned>(slot), nbytes);
  if (!port_.Write(reinterpret_cast<const uint8_t *>(cmd),
                   std::strlen(cmd))) {
    return Fail("err: CDC write wl");
  }
  port_.DrainOutput();

  std::string rx;
  if (!WaitForSubstring(port_, "ok:ready", rx, 2000, true)) {
    if (rx.find("err:") != std::string::npos) {
      return Fail("err: card reply " + SanitizeRx(rx));
    }
    if (rx.empty()) {
      return Fail("err: no CDC reply — use Channel USB cu.usbmodem (not RS485)");
    }
    return Fail("err: no ok:ready (" + SanitizeRx(rx) + ")");
  }

  if (on_progress) {
    on_progress(0.05f);
  }

  rx.clear();
  size_t off = 0;
  while (off < nbytes) {
    const size_t chunk = std::min<size_t>(512, nbytes - off);
    if (!port_.Write(data + off, chunk)) {
      return Fail("err: CDC write payload @" + std::to_string(off));
    }
    off += chunk;
    PollRx(port_, rx);
    if (rx.find("err:") != std::string::npos) {
      return Fail("err: card reply " + SanitizeRx(rx));
    }
    if (on_progress) {
      on_progress(0.05f +
                  0.9f * static_cast<float>(off) /
                      static_cast<float>(nbytes));
    }
  }
  port_.DrainOutput();

  if (rx.find("ok:wave") == std::string::npos) {
    if (!WaitForSubstring(port_, "ok:wave", rx, 3000, true)) {
      if (rx.find("err:") != std::string::npos) {
        return Fail("err: card reply " + SanitizeRx(rx));
      }
      return Fail("err: no ok:wave (" + SanitizeRx(rx) + ")");
    }
  }

  if (on_progress) {
    on_progress(1.f);
  }

  WaveUploadResult out;
  out.ok = true;
  out.slot = slot;
  out.nsamp = static_cast<uint32_t>(nbytes / 2u);
  const auto pos = rx.rfind("ok:wave");
  if (pos != std::string::npos) {
    std::string line = rx.substr(pos);
    const auto end = line.find('\r');
    if (end != std::string::npos) {
      line.resize(end);
    }
    out.message = SanitizeRx(line);
  } else {
    out.message = "ok:wave uploaded";
  }
  return out;
}

WaveUploadResult WaveUploader::UploadFile(
    uint8_t slot,
    const std::string &file_path,
    const std::function<void(float)> &on_progress)
{
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    return Fail("err: cannot open " + file_path);
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  return Upload(slot, data.data(), data.size(), on_progress);
}

} // namespace usb
} // namespace host_io
