#pragma once

#include "voice_bank.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace midi_host
{

/**
 * Channel Card note bank over RS485.
 *
 * Transport is absolute bank snapshots (all 16 slot freqs), not incremental
 * On/Off deltas. Latest snapshot wins on a background thread so a dropped
 * Off is healed on the next key event. Session open sends `c:quiet on` +
 * `e:echo off`; note TX is always wire-speed burst (Effect echo stays off).
 */
class ChannelRs485Out
{
public:
  ChannelRs485Out();
  ~ChannelRs485Out();

  ChannelRs485Out(const ChannelRs485Out&) = delete;
  ChannelRs485Out& operator=(const ChannelRs485Out&) = delete;

  /**
   * Open adapter, bare n0, gain 1 <atten_db>, silence N0–NF, quiet+echo
   * setup when possible, start TX worker.
   * atten_db: CS4304 CH1 atten in dB (0..127), default 6.
   */
  void Open(const std::string& serial_path,
            uint32_t baud = 115200,
            uint32_t atten_db = 6);

  void Close();

  /** Publish the current VoiceBank as the next absolute RS485 snapshot. */
  void ApplyBankEvent(const BankEvent& event, const VoiceBank& bank);

  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return atten_db_; }
  /** True when note TX is wire-speed burst (always after a successful Open). */
  bool BurstNotes() const;
  /** True when this session got ok: to e:echo off. */
  bool EffectEchoDisabled() const;
  /** True when this session issued a successful c:quiet on. */
  bool QuietReplies() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
  uint32_t atten_db_ = 6;
};

} // namespace midi_host
