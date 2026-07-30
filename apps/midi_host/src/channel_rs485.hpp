#pragma once

#include "voice_bank.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace midi_host
{

/**
 * Channel Card note bank over RS485.
 * Note commands are queued and sent on a background thread so the MIDI
 * loop never blocks on bus turnaround / reply waits.
 */
class ChannelRs485Out
{
public:
  ChannelRs485Out();
  ~ChannelRs485Out();

  ChannelRs485Out(const ChannelRs485Out&) = delete;
  ChannelRs485Out& operator=(const ChannelRs485Out&) = delete;

  /**
   * Open adapter, bare n0, gain 1 <atten_db>, silence N0–NF, start TX worker.
   * atten_db: CS4304 CH1 atten in dB (0..127), default 6.
   */
  void Open(const std::string& serial_path,
            uint32_t baud = 115200,
            uint32_t atten_db = 6);

  void Close();

  /** Enqueue one note command (returns immediately). */
  void ApplyBankEvent(const BankEvent& event, const VoiceBank& bank);

  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return atten_db_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
  uint32_t atten_db_ = 6;
};

} // namespace midi_host
