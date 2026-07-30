#pragma once

#include "voice_bank.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace midi_host
{

/**
 * Sends VoiceBank events to the Channel Card note bank over RS485
 * (n0..nf <Hz> / n0..nf 0), reusing apps/console wire protocol.
 */
class ChannelRs485Out
{
public:
  ChannelRs485Out();
  ~ChannelRs485Out();

  ChannelRs485Out(const ChannelRs485Out&) = delete;
  ChannelRs485Out& operator=(const ChannelRs485Out&) = delete;

  /** Open adapter and send session defaults (bare n0). Throws on failure. */
  void Open(const std::string& serial_path, uint32_t baud = 115200);

  void Close();

  void ApplyBankEvent(const BankEvent& event);

  std::string Path() const { return path_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
};

} // namespace midi_host
