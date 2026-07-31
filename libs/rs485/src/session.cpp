#include "rs485/session.hpp"

#include <cstdio>

namespace rs485 {

Session::Session() = default;

Session::~Session() { Close(); }

ExchangeResult Session::RequireOk(Target t, const std::string &cmd) {
  last_ = link_->Send(t, cmd);
  return last_;
}

ExchangeResult Session::Open(const std::string &path, const SessionOptions &opts) {
  Close();
  opts_ = opts;
  if (opts_.atten_db > 127u) {
    last_.status = Status::Err;
    std::snprintf(last_.err_code, sizeof(last_.err_code), "range");
    std::snprintf(last_.raw, sizeof(last_.raw), "atten_db must be 0..127");
    return last_;
  }

  port_.SetManualRtsControl(opts_.manual_rts);
  if (!port_.Open(path, opts_.baud)) {
    last_.status = Status::IoError;
    std::snprintf(last_.raw, sizeof(last_.raw), "%s", port_.LastError().c_str());
    return last_;
  }
  path_ = path;

  LinkOptions lo;
  lo.reply_timeout_ms = opts_.reply_timeout_ms;
  lo.retries = opts_.retries;
  lo.idle_gap_ms = opts_.idle_gap_ms;
  lo.post_tx_settle_ms = opts_.post_tx_settle_ms;
  lo.post_ack_settle_ms = opts_.post_ack_settle_ms;
  lo.late_ack_grace_ms = opts_.late_ack_grace_ms;
  lo.rx_idle_ms = opts_.rx_idle_ms;
  lo.rx_idle_max_ms = opts_.rx_idle_max_ms;
  link_ = std::make_unique<Link>(port_, lo);

  /* Recover sticky quiet from a killed prior session. */
  if (!RequireOk(Target::Channel, "quiet off").ok()) {
    /* Bare n0 also clears quiet — try once more via n0 later. */
  }

  if (opts_.effect_echo != EffectEcho::Leave) {
    const char *echo_cmd =
        (opts_.effect_echo == EffectEcho::On) ? "echo on" : "echo off";
    ExchangeResult echo = RequireOk(Target::Effect, echo_cmd);
    if (!echo.ok()) {
      if (!opts_.allow_missing_effect) {
        bus_fault_ = true;
        open_ = false;
        link_.reset();
        port_.Close();
        path_.clear();
        return last_;
      }
      echo_disabled_ = false;
    } else {
      echo_disabled_ = (opts_.effect_echo == EffectEcho::Off);
    }
  }

  if (!RequireOk(Target::Channel, "n0").ok()) {
    bus_fault_ = true;
    open_ = false;
    link_.reset();
    port_.Close();
    path_.clear();
    return last_;
  }

  char gain_cmd[32];
  std::snprintf(gain_cmd, sizeof(gain_cmd), "gain 1 %u",
                static_cast<unsigned>(opts_.atten_db));
  if (!RequireOk(Target::Channel, gain_cmd).ok()) {
    bus_fault_ = true;
    open_ = false;
    link_.reset();
    port_.Close();
    path_.clear();
    return last_;
  }

  if (!RequireOk(Target::Channel, "silence").ok()) {
    bus_fault_ = true;
    open_ = false;
    link_.reset();
    port_.Close();
    path_.clear();
    return last_;
  }

  open_ = true;
  bus_fault_ = false;
  last_.status = Status::Ok;
  return last_;
}

bool Session::SoftRecover() {
  if (!link_ || !port_.IsOpen()) {
    return false;
  }
  LinkOptions lo = link_->Options();
  const uint32_t saved_to = lo.reply_timeout_ms;
  const int saved_retries = lo.retries;
  /* Silence is more important than note latency — give the card time. */
  lo.reply_timeout_ms = 800;
  lo.retries = 2;
  link_->SetOptions(lo);
  const ExchangeResult sil = link_->Send(Target::Channel, "silence");
  (void)link_->Send(Target::Channel, "quiet off");
  lo.reply_timeout_ms = saved_to;
  lo.retries = saved_retries;
  link_->SetOptions(lo);
  last_ = sil;
  if (sil.ok()) {
    bus_fault_ = false;
    return true;
  }
  /* ACK path failed (common under chord load when a reply was dropped).
   * Blind-silence so voices cannot hang forever behind bus_fault. */
  ForceClearBus();
  bus_fault_ = false;
  return true;
}

void Session::ForceClearBus() {
  if (!link_ || !port_.IsOpen()) {
    return;
  }
  port_.FlushInput();
  (void)link_->SendBlind(Target::Channel, "silence");
  (void)link_->SendBlind(Target::Channel, "silence");
  (void)link_->SendBlind(Target::Channel, "quiet off");
  port_.FlushInput();
}

void Session::Close() {
  if (link_ && port_.IsOpen()) {
    SoftRecover();
  }
  link_.reset();
  port_.Close();
  path_.clear();
  open_ = false;
  echo_disabled_ = false;
}

ExchangeResult Session::Exec(Target target, const std::string &command) {
  if (!open_ || !link_) {
    last_.status = Status::IoError;
    std::snprintf(last_.raw, sizeof(last_.raw), "session not open");
    return last_;
  }
  last_ = link_->Send(target, command);
  if (last_.status == Status::IoError) {
    bus_fault_ = true;
  }
  return last_;
}

ExchangeResult Session::SetNote(uint8_t slot, uint16_t hz) {
  if (slot > 15u) {
    last_.status = Status::Err;
    std::snprintf(last_.err_code, sizeof(last_.err_code), "range");
    return last_;
  }
  if (hz != 0u && (hz < 20u || hz >= 20000u)) {
    last_.status = Status::Err;
    std::snprintf(last_.err_code, sizeof(last_.err_code), "range");
    return last_;
  }
  const char hex = (slot < 10u) ? static_cast<char>('0' + slot)
                                : static_cast<char>('a' + (slot - 10u));
  char cmd[24];
  std::snprintf(cmd, sizeof(cmd), "n%c %u", hex, static_cast<unsigned>(hz));
  return Exec(Target::Channel, cmd);
}

ExchangeResult Session::Silence() {
  return Exec(Target::Channel, "silence");
}

ExchangeResult Session::Gain(uint8_t ch, uint8_t atten_db) {
  if (ch < 1u || ch > 4u || atten_db > 127u) {
    last_.status = Status::Err;
    std::snprintf(last_.err_code, sizeof(last_.err_code), "range");
    return last_;
  }
  char cmd[32];
  std::snprintf(cmd, sizeof(cmd), "gain %u %u", static_cast<unsigned>(ch),
                static_cast<unsigned>(atten_db));
  return Exec(Target::Channel, cmd);
}

} // namespace rs485
