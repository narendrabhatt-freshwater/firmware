#include "wave_cdc.hpp"

#include "rs485/serial_port.hpp"

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
    const size_t n = port.ReadTimeout(buf, sizeof(buf), 50);
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

} // namespace

bool WaveCdc_LoadRaw(const std::string &cdc_path,
                     int slot,
                     const std::string &file_path,
                     LogBuffer &log,
                     const std::function<void(float)> &on_progress)
{
  if (cdc_path.empty()) {
    log.Push("err: no USB CDC path (Channel Card cu.usbmodem…)");
    return false;
  }
  if (LooksLikeRs485Adapter(cdc_path)) {
    log.Push("err: that looks like the RS485 adapter — pick Channel "
             "cu.usbmodem* / ttyACM* for wave upload");
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

  rs485::SerialPort port;
  if (!port.Open(cdc_path, 115200)) {
    log.Push("err: CDC open " + port.LastError());
    return false;
  }

  port.SetDtr(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  {
    std::string boot;
    (void)WaitForSubstring(port, "ready", boot, 1500, false);
  }
  port.FlushInput();

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:wl %d %zu\r", slot, n);
  if (!port.Write(reinterpret_cast<const uint8_t *>(cmd), std::strlen(cmd))) {
    log.Push("err: CDC write wl");
    return false;
  }
  port.DrainOutput();

  std::string rx;
  if (!WaitForSubstring(port, "ok:ready", rx, 5000, true)) {
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

  size_t off = 0;
  while (off < n) {
    const size_t chunk = std::min<size_t>(512, n - off);
    if (!port.Write(data.data() + off, chunk)) {
      log.Push("err: CDC write payload @" + std::to_string(off));
      return false;
    }
    off += chunk;
    if (on_progress) {
      on_progress(0.05f + 0.9f * static_cast<float>(off) / static_cast<float>(n));
    }
  }
  port.DrainOutput();

  rx.clear();
  if (!WaitForSubstring(port, "ok:wave", rx, 10000, true)) {
    if (rx.find("err:") != std::string::npos) {
      log.Push("err: card reply " + SanitizeRx(rx));
    } else {
      log.Push("err: no ok:wave (" + SanitizeRx(rx) + ")");
    }
    return false;
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
