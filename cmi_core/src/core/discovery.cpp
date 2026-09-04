#include "detail.hpp"

#include "cardlink/serial_port.hpp"
#include "cardlink/usb/cdc_port.hpp"

#include <RtAudio.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace cmi {
namespace {

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool Contains(const std::string &value, const char *needle)
{
  return Lower(value).find(Lower(needle)) != std::string::npos;
}

bool IsChannelCdc(const std::string &path)
{
  return Contains(path, "chcard") || Contains(path, "channel") ||
         Contains(path, "usbmodem") || Contains(path, "ttyacm");
}

bool IsRs485(const std::string &path)
{
  if (Contains(path, "chcard") || Contains(path, "efcard")) return false;
  return cardlink::usb::LooksLikeRs485AdapterPath(path) ||
         Contains(path, "usb-uart") || Contains(path, "usb_uart") ||
         Contains(path, "serial/by-id");
}

template <typename T>
void SortUnique(std::vector<T> &items)
{
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
}

std::string Join(const std::vector<std::string> &values)
{
  std::ostringstream out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  return out.str();
}

std::vector<std::string> AudioDevices()
{
  std::vector<std::string> result;
  try {
    RtAudio audio;
    for (unsigned id : audio.getDeviceIds()) {
      const RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
      if (info.outputChannels >= 21u &&
          (Contains(info.name, "channel card") ||
           Contains(info.name, "freshwater"))) {
        result.push_back(info.name);
      }
    }
  } catch (...) {
    // The report below turns an empty list into an actionable diagnostic.
  }
  SortUnique(result);
  return result;
}

void AddPermissionDiagnostics(SetupReport &report)
{
#if !defined(_WIN32)
  for (const std::string *path :
       {&report.params.rs485_port, &report.params.channel_cdc_port}) {
    std::error_code ec;
    if (path->empty() || !std::filesystem::exists(*path, ec) || ec) continue;
    if (::access(path->c_str(), R_OK | W_OK) != 0) {
      report.diagnostics.push_back(
          "no read/write permission for " + *path +
          "; on Ubuntu/Debian add the user to dialout, sign out, and back in");
      if (report.result.ok()) {
        report.result = {ErrorCode::IoError,
                         "serial device permission denied", {}};
      }
    }
  }
#else
  (void)report;
#endif
}

std::vector<std::string> NormalizeSerialPorts(
    const std::vector<std::string> &ports)
{
  // Linux normally exposes the same adapter as both /dev/ttyUSB* and a
  // stable /dev/serial/by-id symlink. Keep one candidate and prefer by-id.
  std::vector<std::pair<std::string, std::string>> normalized;
  for (const auto &path : ports) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    const std::string key = ec ? path : canonical.string();
    auto existing = std::find_if(
        normalized.begin(), normalized.end(),
        [&key](const auto &item) { return item.first == key; });
    if (existing == normalized.end()) {
      normalized.emplace_back(key, path);
    } else if (Contains(path, "/dev/serial/by-id/") &&
               !Contains(existing->second, "/dev/serial/by-id/")) {
      existing->second = path;
    }
  }
  std::vector<std::string> result;
  for (const auto &item : normalized) result.push_back(item.second);
  SortUnique(result);
  return result;
}

} // namespace

namespace detail {

SetupReport BuildSetupReport(CoreParams overrides,
                             DiscoveryOptions options,
                             std::vector<std::string> serial_ports,
                             std::vector<std::string> audio_devices,
                             std::vector<MidiPort> midi_ports)
{
  SetupReport report;
  report.params = std::move(overrides);
  report.channel_audio_devices = std::move(audio_devices);
  report.midi_ports = std::move(midi_ports);

  for (const auto &path : serial_ports) {
    if (IsRs485(path)) report.rs485_ports.push_back(path);
    if (IsChannelCdc(path)) report.channel_cdc_ports.push_back(path);
  }
  SortUnique(report.rs485_ports);
  SortUnique(report.channel_cdc_ports);

  bool ready = true;
  if (report.params.rs485_port.empty()) {
    if (report.rs485_ports.size() == 1u) {
      report.params.rs485_port = report.rs485_ports.front();
    } else {
      ready = false;
      report.diagnostics.push_back(
          report.rs485_ports.empty()
              ? "no RS485 adapter found; connect one or set CoreParams::rs485_port"
              : "multiple RS485 adapters found (" + Join(report.rs485_ports) +
                    "); set CoreParams::rs485_port");
    }
  }
  if (report.params.channel_cdc_port.empty()) {
    std::vector<std::string> named;
    for (const auto &path : report.channel_cdc_ports) {
      if (Contains(path, "chcard") || Contains(path, "channel"))
        named.push_back(path);
    }
    const auto &choices = named.empty() ? report.channel_cdc_ports : named;
    if (choices.size() == 1u) {
      report.params.channel_cdc_port = choices.front();
    } else {
      ready = false;
      report.diagnostics.push_back(
          choices.empty()
              ? "no Channel Card CDC port found; connect the running card or set CoreParams::channel_cdc_port"
              : "multiple Channel Card CDC ports found (" + Join(choices) +
                    "); set CoreParams::channel_cdc_port");
    }
  }
  if (report.params.channel_audio_device.empty()) {
    if (report.channel_audio_devices.size() == 1u) {
      report.params.channel_audio_device =
          report.channel_audio_devices.front();
    } else if (options.require_audio) {
      ready = false;
      report.diagnostics.push_back(
          report.channel_audio_devices.empty()
              ? "no compatible 21-channel Channel Card audio output found"
              : "multiple compatible Channel Card audio outputs found (" +
                    Join(report.channel_audio_devices) +
                    "); set CoreParams::channel_audio_device");
    }
  }
  if (options.select_midi && report.params.midi_port.empty()) {
    if (report.midi_ports.size() == 1u) {
      report.params.midi_port = report.midi_ports.front().name;
    } else if (report.midi_ports.empty()) {
      report.diagnostics.push_back(
          "no MIDI input found; MIDI remains disabled");
    } else {
      report.diagnostics.push_back(
          "multiple MIDI inputs found; set CoreParams::midi_port to enable one");
    }
  }

  report.result = ready
                      ? Result{ErrorCode::Ok, "host setup is ready", {}}
                      : Result{ErrorCode::NotReady,
                               "host setup needs attention", {}};
  return report;
}

} // namespace detail

SetupReport Core::discover(CoreParams overrides, DiscoveryOptions options)
{
  SetupReport report = detail::BuildSetupReport(
      std::move(overrides), options,
      NormalizeSerialPorts(cardlink::SerialPort::ListPorts()),
      AudioDevices(), listMidiPorts());
  AddPermissionDiagnostics(report);
  return report;
}

} // namespace cmi
