/**
 * @file link.hpp
 * @brief One request/response exchange over a shared RS485 bus.
 */

#ifndef HOST_IO_RS485_LINK_HPP
#define HOST_IO_RS485_LINK_HPP

#include "cardlink/serial_port.hpp"
#include "cardlink/rs485/types.hpp"

#include <string>

namespace cardlink {
namespace rs485 {

class Link {
public:
  explicit Link(cardlink::SerialPort &port, LinkOptions opts = {});

  void SetOptions(const LinkOptions &opts) { opts_ = opts; }
  const LinkOptions &Options() const { return opts_; }

  /**
   * Send `command` to `target`. Prepends address prefix (c: / e: / *:)
   * unless command already has an explicit X: prefix. Terminates with a
   * single CR. Never blocks forever — hard reply timeout + bounded retries.
   */
  ExchangeResult Send(Target target, const std::string &command);

  /**
   * Write a CR-terminated command and drain TX — no reply wait.
   * Used only for bus recovery when ACKs are lost (stuck voices).
   */
  bool SendBlind(Target target, const std::string &command);

  uint32_t TimeoutCount() const { return timeout_count_; }
  uint32_t ErrCount() const { return err_count_; }
  const ExchangeResult &LastResult() const { return last_; }

private:
  cardlink::SerialPort &port_;
  LinkOptions opts_;
  ExchangeResult last_{};
  uint32_t timeout_count_ = 0;
  uint32_t err_count_ = 0;

  bool WriteWire(const uint8_t *bytes, size_t len);
  ExchangeResult ReadTerminalReply(Target expected);
  std::string ReadRawWindow();
  /** Drain RX until idle_ms of silence or max_ms elapsed. */
  void WaitRxIdle();
};

} // namespace rs485
} // namespace cardlink

#endif // HOST_IO_RS485_LINK_HPP
