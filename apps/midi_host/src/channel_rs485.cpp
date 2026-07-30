#include "channel_rs485.hpp"

#include "rs485_link.hpp"
#include "serial_port.hpp"

#include <cstdio>
#include <stdexcept>

namespace midi_host
{

struct ChannelRs485Out::Impl
{
  rs485::SerialPort port;
  std::unique_ptr<rs485::RS485Link> link;
};

ChannelRs485Out::ChannelRs485Out()
    : impl_(std::make_unique<Impl>())
{
}

ChannelRs485Out::~ChannelRs485Out()
{
  Close();
}

void ChannelRs485Out::Open(const std::string& serial_path, uint32_t baud)
{
  Close();
  if (!impl_->port.Open(serial_path, baud)) {
    throw std::runtime_error("RS485 open failed: " + impl_->port.LastError());
  }
  path_ = serial_path;
  impl_->link = std::make_unique<rs485::RS485Link>(impl_->port);

  // Bare n0 = bypass on + gain 1 0 (same as fw rs485 / boot session).
  const rs485::ExchangeResult init =
      impl_->link->Send(rs485::Target::Channel, "n0");
  if (!init.got_reply) {
    throw std::runtime_error(
        "no reply to session n0 — check RS485 wiring / Channel Card power");
  }
}

void ChannelRs485Out::Close()
{
  if (impl_) {
    impl_->link.reset();
    impl_->port.Close();
  }
  path_.clear();
}

void ChannelRs485Out::ApplyBankEvent(const BankEvent& event)
{
  if (!impl_ || !impl_->link) {
    return;
  }

  char cmd[64];
  const char slot_hex = "0123456789abcdef"[event.slot & 0x0Fu];

  switch (event.kind) {
  case BankEventKind::On:
  case BankEventKind::Retrig:
    std::snprintf(cmd, sizeof(cmd), "n%c %.4f", slot_hex, event.freq_hz);
    break;
  case BankEventKind::Off:
  case BankEventKind::Steal:
    std::snprintf(cmd, sizeof(cmd), "n%c 0", slot_hex);
    break;
  }

  const rs485::ExchangeResult r =
      impl_->link->Send(rs485::Target::Channel, cmd);
  if (!r.got_reply) {
    std::fprintf(stderr, "warn: no RS485 reply for %s\n", cmd);
  }
}

} // namespace midi_host
