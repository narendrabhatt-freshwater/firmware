/**
 * @file serial_port.hpp
 * @brief Cross-platform OS serial port (macOS/Linux termios, Windows Win32).
 *
 * Shared by RS485 and USB CDC host paths. Blocking Open/Write/Read with
 * timeout; no background threads. Framing belongs in host_io::rs485::Link
 * or host_io::usb transports — this type only moves bytes.
 */

#ifndef HOST_IO_SERIAL_PORT_HPP
#define HOST_IO_SERIAL_PORT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host_io {

class SerialPort {
public:
  SerialPort();
  ~SerialPort();

  SerialPort(const SerialPort &) = delete;
  SerialPort &operator=(const SerialPort &) = delete;

  /** Opens `path` at `baud`, 8N1, no flow control. Returns false and sets
   * LastError() on failure. */
  bool Open(const std::string &path, uint32_t baud);

  /** Closes the port. Safe to call if not open. */
  void Close();

  bool IsOpen() const { return is_open_; }

  /** Writes exactly `len` bytes (retries short writes internally).
   * Returns false on any I/O error. */
  bool Write(const uint8_t *data, size_t len);

  /** Reads up to `max_len` bytes, waiting up to `timeout_ms` for the first
   * byte. Returns the number of bytes read; 0 means the timeout elapsed
   * with nothing received (bus busy/no reply — not necessarily an error,
   * see Link retry policy). */
  size_t ReadTimeout(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

  /** Discard any bytes already sitting in the OS/driver RX buffer (idle
   * noise, leftover TX-echo, turnaround glitches). Call before each
   * transmit so ReadReply() only sees post-TX traffic. */
  void FlushInput();

  /** Block until the OS has shifted out every byte from the last Write().
   * Needed before a follow-up frame so a "silence" cannot sit behind a
   * stale chord still in the USB-serial TX queue. */
  void DrainOutput();

  /** Some USB-RS485 dongles lack auto-direction and need RTS driven high
   * only while transmitting. Off by default (matches ADAM-4520 and most
   * modern auto-direction dongles, which need no RTS handling at all). */
  void SetManualRtsControl(bool enabled) { manual_rts_ = enabled; }
  bool ManualRtsControl() const { return manual_rts_; }

  /** Only meaningful when ManualRtsControl() is true. */
  void SetRts(bool asserted);

  /**
   * Drive DTR. Needed for USB CDC ACM: TinyUSB tud_cdc_connected() is true
   * only while DTR is asserted, and console replies are gated on that.
   */
  void SetDtr(bool asserted);

  const std::string &LastError() const { return last_error_; }

  /** Best-effort enumeration of likely serial device paths for this OS.
   * Not exhaustive — meant to help `--list` narrow down candidates. */
  static std::vector<std::string> ListPorts();

private:
  struct Impl;
  Impl *impl_ = nullptr;
  bool is_open_ = false;
  bool manual_rts_ = false;
  std::string last_error_;

  void SetError(const std::string &context);
};

} // namespace host_io

#endif // HOST_IO_SERIAL_PORT_HPP
