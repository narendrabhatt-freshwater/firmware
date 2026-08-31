#include "cardlink/rs485/bus.hpp"

#include <cstdio>

namespace cardlink {
namespace rs485 {

Bus::Bus() = default;

Bus::~Bus() { Close(); }

cardproto::Result Bus::Open(const std::string &path, const BusOptions &opts)
{
  Close();
  opts_ = opts;
  if (opts_.atten_db > 127u) {
    last_ = cardproto::Result::LocalErr("range", "atten_db must be 0..127");
    return last_;
  }

  port_.SetManualRtsControl(opts_.manual_rts);
  if (!port_.Open(path, opts_.baud)) {
    last_ = cardproto::Result::IoErr(port_.LastError().c_str());
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
  transport_ = std::make_unique<Rs485Transport>(*link_);
  channel_ = std::make_unique<cardproto::ChannelClient>(*transport_);
  effect_ = std::make_unique<cardproto::EffectClient>(*transport_);

  if (opts_.effect_echo != EffectEcho::Leave) {
    const bool want_on = (opts_.effect_echo == EffectEcho::On);
    cardproto::Result echo = effect_->SetEcho(want_on);
    last_ = echo;
    if (!echo.ok()) {
      if (!opts_.allow_missing_effect) {
        bus_fault_ = true;
        open_ = false;
        channel_.reset();
        effect_.reset();
        transport_.reset();
        link_.reset();
        port_.Close();
        path_.clear();
        return last_;
      }
      echo_disabled_ = false;
    } else {
      echo_disabled_ = !want_on;
      if (!want_on) {
        /* Do not merely trust the setter ACK. Echo on a multidrop bus can
         * corrupt Channel vq traffic, so verify the Effect Card's runtime
         * state and reassert ec 0 if the first query is inconclusive. */
        for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
          const cardproto::Result verify = effect_->GetEcho();
          last_ = verify;
          if (verify.ok() &&
              std::string(verify.raw).find("ec 0") != std::string::npos) {
            echo_disabled_ = true;
            break;
          }
          echo_disabled_ = false;
          echo = effect_->SetEcho(false);
          last_ = echo;
          if (!echo.ok()) {
            break;
          }
        }
      }
    }
  }

  auto fail_close = [this](const cardproto::Result &r) -> cardproto::Result {
    last_ = r;
    bus_fault_ = true;
    open_ = false;
    channel_.reset();
    effect_.reset();
    transport_.reset();
    link_.reset();
    port_.Close();
    path_.clear();
    return last_;
  };

  last_ = channel_->NoteDefaults();
  if (!last_.ok()) {
    return fail_close(last_);
  }

  last_ = channel_->SetGain(1, static_cast<uint8_t>(opts_.atten_db));
  if (!last_.ok()) {
    return fail_close(last_);
  }

  last_ = channel_->AllNotesOff();
  /* Older Berry firmware rejected the harmless startup silence command until
   * all eight RAM-only programs had been uploaded. Keep the bus usable so the
   * GUI can connect first and upload those programs later over CDC. */
  const bool no_program_yet =
      std::string(last_.raw).find("err:no-program") != std::string::npos;
  if (!last_.ok() && !no_program_yet) {
    return fail_close(last_);
  }

  open_ = true;
  bus_fault_ = false;
  last_.status = cardproto::Status::Ok;
  return last_;
}

bool Bus::SoftRecover()
{
  if (!link_ || !port_.IsOpen() || !channel_) {
    return false;
  }
  LinkOptions lo = link_->Options();
  const uint32_t saved_to = lo.reply_timeout_ms;
  const int saved_retries = lo.retries;
  lo.reply_timeout_ms = 800;
  lo.retries = 2;
  link_->SetOptions(lo);
  const cardproto::Result sil = channel_->AllNotesOff();
  lo.reply_timeout_ms = saved_to;
  lo.retries = saved_retries;
  link_->SetOptions(lo);
  last_ = sil;
  if (sil.ok()) {
    bus_fault_ = false;
    return true;
  }
  ForceClearBus();
  bus_fault_ = false;
  return true;
}

void Bus::ForceClearBus()
{
  if (!transport_ || !port_.IsOpen()) {
    return;
  }
  port_.FlushInput();
  (void)transport_->SendBlind(cardproto::Target::Channel, "n off");
  (void)transport_->SendBlind(cardproto::Target::Channel, "n off");
  port_.FlushInput();
}

void Bus::Close()
{
  if (channel_ && port_.IsOpen()) {
    SoftRecover();
  }
  channel_.reset();
  effect_.reset();
  transport_.reset();
  link_.reset();
  port_.Close();
  path_.clear();
  open_ = false;
  echo_disabled_ = false;
}

cardproto::Result Bus::Exec(cardproto::Target target, const std::string &command)
{
  if (!open_ || !transport_) {
    last_ = cardproto::Result::IoErr("bus not open");
    return last_;
  }
  last_ = transport_->Exchange(target, command);
  if (last_.status == cardproto::Status::IoError) {
    bus_fault_ = true;
  }
  return last_;
}

uint32_t Bus::TimeoutCount() const
{
  return link_ ? link_->TimeoutCount() : 0;
}

uint32_t Bus::ErrCount() const { return link_ ? link_->ErrCount() : 0; }

} // namespace rs485
} // namespace cardlink
