#pragma once

#include "log_buffer.hpp"

#include "host_io/usb/wave_upload.hpp"

#include "host_io/serial_port.hpp"

#include <cstdint>
#include <functional>
#include <string>

/**
 * Keeps the Channel USB CDC port open across multiple slot loads.
 * Upload framing lives in host_io::usb::WaveUploader.
 */
class WaveCdcSession
{
public:
  bool Open(const std::string &cdc_path, LogBuffer &log);
  void Close();
  bool IsOpen() const { return port_.IsOpen(); }

  bool LoadFile(int slot,
                const std::string &file_path,
                LogBuffer &log,
                const std::function<void(float)> &on_progress = {});

private:
  host_io::SerialPort port_;
};

/** One-shot helper (opens, loads, closes). Prefer WaveCdcSession for batches. */
bool WaveCdc_LoadRaw(const std::string &cdc_path,
                     int slot,
                     const std::string &file_path,
                     LogBuffer &log,
                     const std::function<void(float)> &on_progress = {});

std::string WaveCdc_PickRawFile();
std::string WaveCdc_PickFolder();
