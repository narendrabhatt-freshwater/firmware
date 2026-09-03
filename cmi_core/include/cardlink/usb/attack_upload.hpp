/**
 * @file attack_upload.hpp
 * @brief USB CDC binary sample (`al`) and logical wavetable (`wl`) uploads.
 */

#ifndef CARDLINK_USB_ATTACK_UPLOAD_HPP
#define CARDLINK_USB_ATTACK_UPLOAD_HPP

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

  /** Upload logical oscillator wave 0..7; the card owns physical placement. */
  AttackUploadResult UploadWavetable(
      uint8_t wave, const uint8_t *data, size_t nbytes,
      const std::function<void(float)> &on_progress = {});

private:
  AttackUploadResult UploadCommand(
      const char *command, const char *completion, uint16_t result_id,
      const uint8_t *data, size_t nbytes,
      const std::function<void(float)> &on_progress);
  cardlink::SerialPort &port_;
};

} // namespace usb
} // namespace cardlink

#endif /* CARDLINK_USB_ATTACK_UPLOAD_HPP */
