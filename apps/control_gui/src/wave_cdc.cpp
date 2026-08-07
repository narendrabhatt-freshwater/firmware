#include "wave_cdc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{

bool LooksLikeRs485Adapter(const std::string &path)
{
  return path.find("usbserial") != std::string::npos ||
         path.find("SLAB_USBtoUART") != std::string::npos ||
         path.find("usb-serial") != std::string::npos ||
         path.find("ttyUSB") != std::string::npos;
}

bool WaitForSubstring(rs485::SerialPort &port,
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
    s += "…";
  }
  return s;
}

std::string ShellTrim(std::string out)
{
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

/** Discard leftover console noise without blocking long. */
void DrainRx(rs485::SerialPort &port)
{
  uint8_t buf[256];
  for (int i = 0; i < 8; ++i) {
    if (port.ReadTimeout(buf, sizeof(buf), 5) == 0) {
      break;
    }
  }
}

/** Non-blocking append of whatever is already in the OS RX buffer. */
void PollRx(rs485::SerialPort &port, std::string &accum)
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

} // namespace

bool WaveCdcSession::Open(const std::string &cdc_path, LogBuffer &log)
{
  Close();
  if (cdc_path.empty()) {
    log.Push("err: no USB CDC path (Channel Card cu.usbmodem…)");
    return false;
  }
  if (LooksLikeRs485Adapter(cdc_path)) {
    log.Push("err: that looks like the RS485 adapter — pick Channel "
             "cu.usbmodem* / ttyACM* for wave upload");
    return false;
  }
  if (!port_.Open(cdc_path, 115200)) {
    log.Push("err: CDC open " + port_.LastError());
    return false;
  }
  port_.SetDtr(true);
  /* Short settle only — do not burn 1.5s waiting for a boot banner. */
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  DrainRx(port_);
  port_.FlushInput();
  warmed_ = true;
  return true;
}

void WaveCdcSession::Close()
{
  port_.Close();
  warmed_ = false;
}

bool WaveCdcSession::LoadFile(int slot,
                              const std::string &file_path,
                              LogBuffer &log,
                              const std::function<void(float)> &on_progress)
{
  if (!port_.IsOpen()) {
    log.Push("err: CDC session not open");
    return false;
  }
  if (slot < 0 || slot > 7) {
    log.Push("err: wave slot 0..7");
    return false;
  }

  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    log.Push("err: cannot open " + file_path);
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  const size_t n = data.size();
  if (n < 2 || (n & 1u) != 0u || n > 32768u) {
    log.Push("err: raw size must be even, 2..32768 bytes (got " +
             std::to_string(n) + ")");
    return false;
  }

  DrainRx(port_);
  port_.FlushInput();

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:wl %d %zu\r", slot, n);
  if (!port_.Write(reinterpret_cast<const uint8_t *>(cmd), std::strlen(cmd))) {
    log.Push("err: CDC write wl");
    return false;
  }
  port_.DrainOutput();

  std::string rx;
  if (!WaitForSubstring(port_, "ok:ready", rx, 2000, true)) {
    if (rx.find("err:") != std::string::npos) {
      log.Push("err: card reply " + SanitizeRx(rx));
    } else if (rx.empty()) {
      log.Push("err: no CDC reply — use Channel USB cu.usbmodem (not RS485), "
               "flash waveform firmware, keep USB plugged");
    } else {
      log.Push("err: no ok:ready (" + SanitizeRx(rx) + ")");
    }
    return false;
  }

  if (on_progress) {
    on_progress(0.05f);
  }

  /* Never DrainRx during/after payload — that discarded ok:wave (empty ()).
   * Accumulate replies while writing; card RX FIFO is only 256 B so keep
   * host chunks modest and poll so we notice completion promptly. */
  rx.clear();
  size_t off = 0;
  while (off < n) {
    const size_t chunk = std::min<size_t>(512, n - off);
    if (!port_.Write(data.data() + off, chunk)) {
      log.Push("err: CDC write payload @" + std::to_string(off));
      return false;
    }
    off += chunk;
    PollRx(port_, rx);
    if (rx.find("err:") != std::string::npos) {
      log.Push("err: card reply " + SanitizeRx(rx));
      return false;
    }
    if (on_progress) {
      on_progress(0.05f + 0.9f * static_cast<float>(off) / static_cast<float>(n));
    }
  }
  port_.DrainOutput();

  if (rx.find("ok:wave") == std::string::npos) {
    if (!WaitForSubstring(port_, "ok:wave", rx, 3000, true)) {
      if (rx.find("err:") != std::string::npos) {
        log.Push("err: card reply " + SanitizeRx(rx));
      } else {
        log.Push("err: no ok:wave (" + SanitizeRx(rx) + ")");
      }
      return false;
    }
  }

  if (on_progress) {
    on_progress(1.f);
  }

  const auto pos = rx.rfind("ok:wave");
  if (pos != std::string::npos) {
    std::string line = rx.substr(pos);
    const auto end = line.find('\r');
    if (end != std::string::npos) {
      line.resize(end);
    }
    log.Push(SanitizeRx(line));
  } else {
    log.Push("ok:wave uploaded");
  }
  return true;
}

bool WaveCdc_LoadRaw(const std::string &cdc_path,
                     int slot,
                     const std::string &file_path,
                     LogBuffer &log,
                     const std::function<void(float)> &on_progress)
{
  WaveCdcSession session;
  if (!session.Open(cdc_path, log)) {
    return false;
  }
  const bool ok = session.LoadFile(slot, file_path, log, on_progress);
  session.Close();
  return ok;
}

std::string WaveCdc_PickRawFile()
{
#if defined(__APPLE__)
  FILE *pipe = popen(
      "osascript -e 'POSIX path of (choose file with prompt \"Raw int16 LE "
      "wave\" of type {\"public.data\",\"public.item\"})' 2>/dev/null",
      "r");
  if (!pipe) {
    return {};
  }
  char buf[1024];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe)) {
    out += buf;
  }
  pclose(pipe);
  return ShellTrim(std::move(out));
#elif defined(__linux__)
  FILE *pipe =
      popen("zenity --file-selection --title='Raw int16 LE wave' 2>/dev/null",
            "r");
  if (!pipe) {
    return {};
  }
  char buf[1024];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe)) {
    out += buf;
  }
  pclose(pipe);
  return ShellTrim(std::move(out));
#else
  return {};
#endif
}

std::string WaveCdc_PickFolder()
{
#if defined(__APPLE__)
  FILE *pipe = popen(
      "osascript -e 'POSIX path of (choose folder with prompt \"Wave bank "
      "folder (w0_*.raw … w7_*.raw)\")' 2>/dev/null",
      "r");
  if (!pipe) {
    return {};
  }
  char buf[1024];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe)) {
    out += buf;
  }
  pclose(pipe);
  return ShellTrim(std::move(out));
#elif defined(__linux__)
  FILE *pipe = popen(
      "zenity --file-selection --directory --title='Wave bank folder' "
      "2>/dev/null",
      "r");
  if (!pipe) {
    return {};
  }
  char buf[1024];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe)) {
    out += buf;
  }
  pclose(pipe);
  return ShellTrim(std::move(out));
#else
  return {};
#endif
}
