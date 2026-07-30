#pragma once

#include "voice_bank.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace midi_host
{

/**
 * Channel Card note bank over RS485.
 * Note commands are coalesced per slot (latest wins) and sent on a
 * background thread so the MIDI loop never blocks on bus turnaround.
 * Coalescing prevents chord note-offs from being dropped under backlog.
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
   * pace_us: delay between TX bytes. Default 1000. Required while Effect
   *   Card echo shares the bus — a burst (0) overlaps that echo and Channel
   *   sees garbled frames like "cjc:n0…". Use 0 only with Effect powered off
   *   and interrupt-driven UART5 RX on Channel.
   */
  void Open(const std::string& serial_path,
            uint32_t baud = 115200,
            uint32_t atten_db = 6,
            uint32_t pace_us = 1000);

  void Close();

  /** Enqueue one note command (returns immediately). */
  void ApplyBankEvent(const BankEvent& event, const VoiceBank& bank);

  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return atten_db_; }
  uint32_t PaceUs() const { return pace_us_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
  uint32_t atten_db_ = 6;
  uint32_t pace_us_ = 1000;
};

} // namespace midi_host
