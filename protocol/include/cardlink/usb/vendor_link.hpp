#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace cardlink {
namespace usb {

/** libusb handle for the Channel Card vendor bulk interface. */
class VendorLink {
public:
  VendorLink();
  ~VendorLink();

  VendorLink(const VendorLink &) = delete;
  VendorLink &operator=(const VendorLink &) = delete;

  bool Open(uint16_t vid, uint16_t pid, std::string &err);
  void Close();
  bool Opened() const;

  /** Fire-and-forget bulk OUT. No ACK. */
  bool Write(const void *data, int nbytes, unsigned timeout_ms,
             std::string &err);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace usb
} // namespace cardlink
