/**
 * @file session.hpp
 * @brief High-level Channel/Effect RS485 session (bootstrap + voice helpers).
 */

#ifndef RS485_SESSION_HPP
#define RS485_SESSION_HPP

#include "rs485/link.hpp"
#include "rs485/serial_port.hpp"
#include "rs485/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace rs485 {

/** What Session::Open sends to Effect for keystroke bus echo. */
enum class EffectEcho : uint8_t {
  Off = 0,  /**< e:echo off (default for MIDI / burst TX) */
  On = 1,   /**< e:echo on */
  Leave = 2 /**< do not send an echo command */
};

struct SessionOptions {
  uint32_t baud = 115200;
  uint32_t atten_db = 6; /**< CH1 gain at open (0..127). */
  uint32_t reply_timeout_ms = 400;
  int retries = 1;
  bool manual_rts = false;
  EffectEcho effect_echo = EffectEcho::Off;
  /** Allow Open to succeed if Effect is absent when echo Off/On is requested. */
  bool allow_missing_effect = false;
};

/**
 * Owns the serial port + link. Production path: quiet off → echo off →
 * n0 / gain / silence, then burst one-by-one note commands with strict ACK.
 */
class Session {
public:
  Session();
  ~Session();

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /** Open adapter and run bootstrap. Returns Ok on success; else last status. */
  ExchangeResult Open(const std::string &path, const SessionOptions &opts = {});

  /** Best-effort silence + quiet off, then close port. */
  void Close();

  bool IsOpen() const { return open_; }
  bool BusFault() const { return bus_fault_; }
  void ClearBusFault() { bus_fault_ = false; }

  ExchangeResult Exec(Target target, const std::string &command);

  /** Integer Hz; 0 = off. Slot 0..15. */
  ExchangeResult SetNote(uint8_t slot, uint16_t hz);
  ExchangeResult NoteOff(uint8_t slot) { return SetNote(slot, 0); }
  ExchangeResult Silence();
  ExchangeResult Gain(uint8_t ch, uint8_t atten_db);

  /** Best-effort silence + quiet off (short timeouts). Returns true if silence ok. */
  bool SoftRecover();
  void MarkBusFault() { bus_fault_ = true; }

  const ExchangeResult &LastResult() const { return last_; }
  uint32_t TimeoutCount() const { return link_ ? link_->TimeoutCount() : 0; }
  uint32_t ErrCount() const { return link_ ? link_->ErrCount() : 0; }
  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return opts_.atten_db; }
  bool EchoDisabled() const { return echo_disabled_; }

  SerialPort &Port() { return port_; }
  Link *GetLink() { return link_.get(); }

private:
  SerialPort port_;
  std::unique_ptr<Link> link_;
  SessionOptions opts_{};
  std::string path_;
  bool open_ = false;
  bool bus_fault_ = false;
  bool echo_disabled_ = false;
  ExchangeResult last_{};

  ExchangeResult RequireOk(Target t, const std::string &cmd);
};

} // namespace rs485

#endif // RS485_SESSION_HPP
