#pragma once

#include <cstdint>
#include <functional>
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

  /** Queue a copied bulk OUT buffer without waiting for its USB completion. */
  bool SubmitWrite(const void *data, int nbytes, unsigned timeout_ms,
                   std::function<void(bool)> completion, std::string &err);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace usb
} // namespace cardlink
