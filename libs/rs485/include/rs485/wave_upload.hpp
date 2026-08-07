/**
 * @file wave_upload.hpp
 * @brief USB CDC binary wave bank upload (`wl` session).
 */

#ifndef RS485_WAVE_UPLOAD_HPP
#define RS485_WAVE_UPLOAD_HPP

#include "rs485/serial_port.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rs485 {

struct WaveUploadResult {
  bool ok = false;
  std::string message; /**< ok:wave … or err:… */
  uint8_t slot = 0;
  uint32_t nsamp = 0;
};

/**
 * Owns nothing — operates on an already-open Channel CDC SerialPort.
 * Sequence: wl → ok:ready → raw int16 LE → ok:wave (no mid-payload ACKs).
 */
class WaveUploader {
public:
  explicit WaveUploader(SerialPort &cdc_port);

  /** Open helper: DTR, short settle, drain. Rejects USB-UART RS485 paths. */
  static bool OpenCdcPort(SerialPort &port,
                          const std::string &cdc_path,
                          std::string &err_out,
                          uint32_t baud = 115200);

  WaveUploadResult Upload(uint8_t slot,
                          const uint8_t *data,
                          size_t nbytes,
                          const std::function<void(float)> &on_progress = {});

  WaveUploadResult UploadFile(
      uint8_t slot,
      const std::string &file_path,
      const std::function<void(float)> &on_progress = {});

private:
  SerialPort &port_;
};

bool LooksLikeRs485AdapterPath(const std::string &path);

} // namespace rs485

#endif /* RS485_WAVE_UPLOAD_HPP */
