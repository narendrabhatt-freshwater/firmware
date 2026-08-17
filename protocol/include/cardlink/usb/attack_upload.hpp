/**
 * @file attack_upload.hpp
 * @brief USB CDC binary attack-head upload (`al` session, ≤ kAttackSamples int32).
 */

#ifndef HOST_IO_USB_ATTACK_UPLOAD_HPP
#define HOST_IO_USB_ATTACK_UPLOAD_HPP

#include "cardlink/serial_port.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace cardlink {
namespace usb {

struct AttackUploadResult {
  bool ok = false;
  std::string message;
  uint16_t wave_id = 0;
};

class AttackUploader {
public:
  explicit AttackUploader(cardlink::SerialPort &cdc_port);

  AttackUploadResult Upload(uint16_t wave_id,
                            const uint8_t *data,
                            size_t nbytes,
                            const std::function<void(float)> &on_progress = {});

  AttackUploadResult UploadFile(
      uint16_t wave_id,
      const std::string &file_path,
      const std::function<void(float)> &on_progress = {});

private:
  cardlink::SerialPort &port_;
};

} // namespace usb
} // namespace cardlink

#endif /* HOST_IO_USB_ATTACK_UPLOAD_HPP */
