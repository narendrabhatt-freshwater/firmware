#include "wave_cdc.hpp"

#include <cstdio>
#include <string>

namespace
{

std::string ShellTrim(std::string out)
{
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

} // namespace

bool WaveCdcSession::Open(const std::string &cdc_path, LogBuffer &log)
{
  Close();
  std::string err;
  if (!cardlink::usb::WaveUploader::OpenCdcPort(port_, cdc_path, err)) {
    log.Push(err);
    return false;
  }
  return true;
}

void WaveCdcSession::Close() { port_.Close(); }

bool WaveCdcSession::LoadFile(int slot,
                              const std::string &file_path,
                              LogBuffer &log,
                              const std::function<void(float)> &on_progress)
{
  if (slot < 0 || slot > 7) {
    log.Push("err: wave slot 0..7");
    return false;
  }
  cardlink::usb::WaveUploader up(port_);
  const auto r = up.UploadFile(static_cast<uint8_t>(slot), file_path,
                               on_progress);
  log.Push(r.message);
  return r.ok;
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
