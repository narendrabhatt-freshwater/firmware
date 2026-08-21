/**
 * @file bus.hpp
 * @brief Owns RS485 port + link; bootstrap; protocol Channel/Effect clients.
 */

#ifndef HOST_IO_RS485_BUS_HPP
#define HOST_IO_RS485_BUS_HPP

#include "protocol/channel.hpp"
#include "protocol/effect.hpp"
#include "protocol/result.hpp"

#include "host_io/rs485/link.hpp"
#include "host_io/rs485/transport.hpp"
#include "host_io/serial_port.hpp"
#include "host_io/rs485/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace host_io {
namespace rs485 {

enum class EffectEcho : uint8_t {
  Off = 0,
  On = 1,
  Leave = 2,
};

struct BusOptions {
  uint32_t baud = 921600;
  uint32_t atten_db = 6;
  uint32_t reply_timeout_ms = 400;
  int retries = 1;
  uint32_t idle_gap_ms = 50;
  uint32_t post_tx_settle_ms = 0;
  uint32_t post_ack_settle_ms = 0;
  uint32_t late_ack_grace_ms = 120;
  uint32_t rx_idle_ms = 5;
  uint32_t rx_idle_max_ms = 80;
  bool manual_rts = false;
  EffectEcho effect_echo = EffectEcho::Off;
  bool allow_missing_effect = false;
};

/** Production RS485 host session: ec → n0 / g / n 0, then typed clients. */
class Bus {
public:
  Bus();
  ~Bus();

  Bus(const Bus &) = delete;
  Bus &operator=(const Bus &) = delete;

  protocol::Result Open(const std::string &path, const BusOptions &opts = {});
  void Close();

  bool IsOpen() const { return open_; }
  bool BusFault() const { return bus_fault_; }
  void ClearBusFault() { bus_fault_ = false; }
  void MarkBusFault() { bus_fault_ = true; }

  protocol::ChannelClient &Channel() { return *channel_; }
  protocol::EffectClient &Effect() { return *effect_; }
  protocol::IConsoleTransport &Transport() { return *transport_; }

  protocol::Result Exec(protocol::Target target, const std::string &command);

  bool SoftRecover();
  void ForceClearBus();

  const protocol::Result &LastResult() const { return last_; }
  uint32_t TimeoutCount() const;
  uint32_t ErrCount() const;
  std::string Path() const { return path_; }
  uint32_t AttenDb() const { return opts_.atten_db; }
  bool EchoDisabled() const { return echo_disabled_; }

  host_io::SerialPort &Port() { return port_; }
  Link *GetLink() { return link_.get(); }

private:
  host_io::SerialPort port_;
  std::unique_ptr<Link> link_;
  std::unique_ptr<Rs485Transport> transport_;
  std::unique_ptr<protocol::ChannelClient> channel_;
  std::unique_ptr<protocol::EffectClient> effect_;
  BusOptions opts_{};
  std::string path_;
  bool open_ = false;
  bool bus_fault_ = false;
  bool echo_disabled_ = false;
  protocol::Result last_{};
};

} // namespace rs485
} // namespace host_io

#endif /* HOST_IO_RS485_BUS_HPP */
