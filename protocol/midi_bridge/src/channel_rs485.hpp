#pragma once

#include "voice_bank.hpp"

#include "cardlink/rs485/bus.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace midi_host
{

/**
 * Channel Card notes over RS485 — one ASCII nX command per event, strict ACK.
 * Also polls `vq` while voices may still be releasing so the host can stop
 * UAC dry streams when the card reports idle.
 */
class ChannelRs485Out
{
public:
  /** Called when vq reports a voice idle (bit clear) after note activity. */
  using IdleHandler = std::function<void(uint8_t slot)>;

  ChannelRs485Out();
  ~ChannelRs485Out();

  ChannelRs485Out(const ChannelRs485Out &) = delete;
  ChannelRs485Out &operator=(const ChannelRs485Out &) = delete;

  /**
   * Open adapter, bootstrap (optional ec, n0, g, n 0),
   * start TX worker. atten_db: CS4304 CH1 atten 0..127.
   * effect_echo: Off (default), On, or Leave (no e:ec command).
   */
  void Open(const std::string &serial_path,
            uint32_t baud = 460800,
            uint32_t atten_db = 6,
            cardlink::rs485::EffectEcho effect_echo = cardlink::rs485::EffectEcho::Off);

  void Close();

  /** Queue On/Off/Retrig as one-by-one SetNote/NoteOff with ACK. */
  void ApplyBankEvent(const BankEvent &event, const VoiceBank &bank);

  /** Optional: Silence UAC voice when card reports idle after release. */
  void SetIdleHandler(IdleHandler handler);

  /** Queue `ar <id> <Hz>` — attack-head root for on-card pitch. */
  void SetRootHz(uint8_t wave_id, double hz);

  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return atten_db_; }
  bool EffectEchoDisabled() const;
  /** True after missing ACK / I/O — note output has stopped. */
  bool BusFault() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
  uint32_t atten_db_ = 6;
};

} // namespace midi_host
