#pragma once

#include "log_buffer.hpp"
#include "rs485/serial_port.hpp"

#include <cstdint>
#include <functional>
#include <string>

/**
 * Keeps the Channel USB CDC port open across multiple slot loads.
 * USB FS (~12 Mbit/s) can move 32 KiB in tens of ms; reopen + settle
 * delays were the real cost of "Upload all".
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
  rs485::SerialPort port_;
  bool warmed_ = false;
};

/** One-shot helper (opens, loads, closes). Prefer WaveCdcSession for batches. */
bool WaveCdc_LoadRaw(const std::string &cdc_path,
                     int slot,
                     const std::string &file_path,
                     LogBuffer &log,
                     const std::function<void(float)> &on_progress = {});

std::string WaveCdc_PickRawFile();
std::string WaveCdc_PickFolder();
