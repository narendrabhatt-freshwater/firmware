/**
 * @file serial_port_win32.cpp
 * @brief Win32 comm-port SerialPort backend for Windows.
 *
 * Untested on real Windows hardware as of this writing (no Windows box in
 * the loop yet) — written against the documented Win32 comm API
 * (CreateFile/GetCommState/SetCommState/SetCommTimeouts). Re-verify against
 * real hardware once a Windows dev machine + adapter are available.
 */

#include "serial_port.hpp"

#include <windows.h>

#include <string>

namespace rs485 {

struct SerialPort::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
};

SerialPort::SerialPort() : impl_(new Impl) {}

SerialPort::~SerialPort() {
  Close();
  delete impl_;
}

void SerialPort::SetError(const std::string &context) {
  DWORD err = GetLastError();
  char msg[256] = {0};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, err, 0, msg, sizeof(msg), nullptr);
  last_error_ = context + ": " + msg;
}

namespace {

/** "COM3" opens fine with CreateFileA, but ports above COM9 need the
 * "\\\\.\\COMn" form — always use it, it works for both ranges. */
std::string ToDevicePath(const std::string &path) {
  if (path.rfind("\\\\.\\", 0) == 0)
    return path;
  return "\\\\.\\" + path;
}

} // namespace

bool SerialPort::Open(const std::string &path, uint32_t baud) {
  Close();

  HANDLE h = CreateFileA(ToDevicePath(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    SetError("CreateFile(" + path + ")");
    return false;
  }

  DCB dcb;
  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(h, &dcb)) {
    SetError("GetCommState");
    CloseHandle(h);
    return false;
  }

  dcb.BaudRate = baud;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_DISABLE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fRtsControl = RTS_CONTROL_DISABLE; /* manual RTS toggled via SetRts() */

  if (!SetCommState(h, &dcb)) {
    SetError("SetCommState");
    CloseHandle(h);
    return false;
  }

  COMMTIMEOUTS timeouts;
  memset(&timeouts, 0, sizeof(timeouts));
  /* ReadTimeout()'s timeout_ms is passed in per-call via
   * SetCommTimeouts() right before each ReadFile() — see ReadTimeout()
   * below — so these defaults just need to be "don't block forever". */
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = 1;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 2000;
  if (!SetCommTimeouts(h, &timeouts)) {
    SetError("SetCommTimeouts");
    CloseHandle(h);
    return false;
  }

  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

  impl_->handle = h;
  is_open_ = true;
  return true;
}

void SerialPort::Close() {
  if (impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
  is_open_ = false;
}

bool SerialPort::Write(const uint8_t *data, size_t len) {
  if (!is_open_) {
    last_error_ = "Write: port not open";
    return false;
  }
  DWORD written = 0;
  if (!WriteFile(impl_->handle, data, static_cast<DWORD>(len), &written,
                 nullptr)) {
    SetError("WriteFile");
    return false;
  }
  return written == len;
}

size_t SerialPort::ReadTimeout(uint8_t *buf, size_t max_len,
                                uint32_t timeout_ms) {
  if (!is_open_ || max_len == 0)
    return 0;

  COMMTIMEOUTS timeouts;
  memset(&timeouts, 0, sizeof(timeouts));
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = timeout_ms == 0 ? 1 : timeout_ms;
  SetCommTimeouts(impl_->handle, &timeouts);

  DWORD read = 0;
  if (!ReadFile(impl_->handle, buf, static_cast<DWORD>(max_len), &read,
               nullptr)) {
    return 0;
  }
  return static_cast<size_t>(read);
}

void SerialPort::SetRts(bool asserted) {
  if (!is_open_ || !manual_rts_)
    return;
  EscapeCommFunction(impl_->handle, asserted ? SETRTS : CLRRTS);
}

std::vector<std::string> SerialPort::ListPorts() {
  std::vector<std::string> ports;
  char devices[65536];
  DWORD n = QueryDosDeviceA(nullptr, devices, sizeof(devices));
  if (n == 0)
    return ports;

  for (char *p = devices; *p != '\0'; p += strlen(p) + 1) {
    if (strncmp(p, "COM", 3) == 0) {
      ports.emplace_back(p);
    }
  }
  return ports;
}

} // namespace rs485
