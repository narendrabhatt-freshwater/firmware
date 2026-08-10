/**
 * @file serial_port_posix.cpp
 * @brief termios-based SerialPort backend for macOS and Linux.
 */

#include "cardlink/serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

namespace cardlink {

struct SerialPort::Impl {
  int fd = -1;
};

SerialPort::SerialPort() : impl_(new Impl) {}

SerialPort::~SerialPort() {
  Close();
  delete impl_;
}

void SerialPort::SetError(const std::string &context) {
  last_error_ = context + ": " + std::strerror(errno);
}

namespace {

/** Maps a requested baud to a termios speed_t constant for the common
 * rates; returns 0 if not one of the fixed constants (caller falls back
 * to a platform-specific arbitrary-baud path, e.g. IOSSIOSPEED on macOS). */
speed_t StandardSpeed(uint32_t baud) {
  switch (baud) {
  case 1200:
    return B1200;
  case 2400:
    return B2400;
  case 4800:
    return B4800;
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
#ifdef B230400
  case 230400:
    return B230400;
#endif
#ifdef B460800
  case 460800:
    return B460800;
#endif
#ifdef B921600
  case 921600:
    return B921600;
#endif
  default:
    return 0;
  }
}

} // namespace

bool SerialPort::Open(const std::string &path, uint32_t baud) {
  Close();

  int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    SetError("open(" + path + ")");
    return false;
  }

  /* Drop O_NONBLOCK now that the open() race (which needs it, e.g. to
   * open a port with no DCD) is past — ReadTimeout()/Write() manage
   * blocking themselves via poll(). */
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  struct termios tio;
  if (tcgetattr(fd, &tio) != 0) {
    SetError("tcgetattr");
    ::close(fd);
    return false;
  }

  cfmakeraw(&tio); /* 8N1, no echo, no signal chars, no software flow */
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS); /* no HW flow control */
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0; /* ReadTimeout() uses poll(), not VTIME */

  speed_t speed = StandardSpeed(baud);
  bool used_standard_speed = (speed != 0);
  if (used_standard_speed) {
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
  }

  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    SetError("tcsetattr");
    ::close(fd);
    return false;
  }

  if (!used_standard_speed) {
#if defined(__APPLE__)
    /* Arbitrary baud rate not in the fixed termios table. */
    speed_t requested = static_cast<speed_t>(baud);
    if (ioctl(fd, IOSSIOSPEED, &requested) != 0) {
      SetError("ioctl(IOSSIOSPEED)");
      ::close(fd);
      return false;
    }
#else
    SetError("unsupported baud rate " + std::to_string(baud));
    ::close(fd);
    return false;
#endif
  }

  tcflush(fd, TCIOFLUSH);

  impl_->fd = fd;
  is_open_ = true;
  return true;
}

void SerialPort::Close() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  is_open_ = false;
}

bool SerialPort::Write(const uint8_t *data, size_t len) {
  if (!is_open_) {
    last_error_ = "Write: port not open";
    return false;
  }
  size_t written = 0;
  while (written < len) {
    ssize_t n = ::write(impl_->fd, data + written, len - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      SetError("write");
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

size_t SerialPort::ReadTimeout(uint8_t *buf, size_t max_len,
                                uint32_t timeout_ms) {
  if (!is_open_ || max_len == 0)
    return 0;

  struct pollfd pfd;
  pfd.fd = impl_->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  int rc = poll(&pfd, 1, static_cast<int>(timeout_ms));
  if (rc <= 0)
    return 0; /* timeout, or a poll() error we treat as "nothing arrived" */

  ssize_t n = ::read(impl_->fd, buf, max_len);
  if (n <= 0)
    return 0;
  return static_cast<size_t>(n);
}

void SerialPort::FlushInput() {
  if (!is_open_)
    return;
  tcflush(impl_->fd, TCIFLUSH);
}

void SerialPort::DrainOutput() {
  if (!is_open_)
    return;
  (void)tcdrain(impl_->fd);
}

void SerialPort::SetRts(bool asserted) {
  if (!is_open_ || !manual_rts_)
    return;
  int status = 0;
  if (ioctl(impl_->fd, TIOCMGET, &status) != 0)
    return;
  if (asserted)
    status |= TIOCM_RTS;
  else
    status &= ~TIOCM_RTS;
  ioctl(impl_->fd, TIOCMSET, &status);
}

void SerialPort::SetDtr(bool asserted) {
  if (!is_open_)
    return;
  int status = 0;
  if (ioctl(impl_->fd, TIOCMGET, &status) != 0)
    return;
  if (asserted)
    status |= TIOCM_DTR;
  else
    status &= ~TIOCM_DTR;
  ioctl(impl_->fd, TIOCMSET, &status);
}

std::vector<std::string> SerialPort::ListPorts() {
  static const char *kPatterns[] = {
#if defined(__APPLE__)
      "/dev/cu.usbserial-*", "/dev/cu.usbmodem*", "/dev/cu.SLAB_USBtoUART*",
#else
      "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/serial/by-id/*",
#endif
  };

  std::vector<std::string> ports;
  for (const char *pattern : kPatterns) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(pattern, 0, nullptr, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++) {
        ports.emplace_back(g.gl_pathv[i]);
      }
    }
    globfree(&g);
  }
  return ports;
}

} // namespace cardlink
