#ifndef CARDLINK_VM_UPLOADER_HPP
#define CARDLINK_VM_UPLOADER_HPP

#include "cardlink/serial_port.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace cardlink::vm {

struct VmUploadResult {
  bool ok = false;
  std::string message;
};

struct VmStatus {
  bool ok = false;
  bool active = false;
  uint8_t voice = 0;
  uint32_t target_id = 0;
  uint16_t target_version = 0;
  uint32_t fault = 0;
  std::string message;
};

class VmUploader {
public:
  explicit VmUploader(cardlink::SerialPort &cdc_port);
  VmUploadResult Upload(uint8_t voice, const uint8_t *program, size_t size,
                        const std::function<void(float)> &on_progress = {});
  VmUploadResult UploadFile(
      uint8_t voice, const std::string &path,
      const std::function<void(float)> &on_progress = {});
  /** Silence the card, wait for release, then upload one program to voices 0-7. */
  VmUploadResult UploadAll(
      const uint8_t *program, size_t size,
      const std::function<void(uint8_t, float)> &on_progress = {});
  VmStatus Status(uint8_t voice);

private:
  cardlink::SerialPort &port_;
};

} // namespace cardlink::vm

#endif
