#include "cardlink/usb/vendor_link.hpp"

#include "cardlink/usb/stream_proto.hpp"

#if defined(__has_include)
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#else
#error "libusb-1.0 headers not found"
#endif
#else
#include <libusb.h>
#endif

#include <mutex>

namespace cardlink {
namespace usb {

struct VendorLink::Impl {
  libusb_context *ctx = nullptr;
  libusb_device_handle *handle = nullptr;
  std::mutex mu;
};

VendorLink::VendorLink() : impl_(std::make_unique<Impl>()) {}

VendorLink::~VendorLink() { Close(); }

bool VendorLink::Opened() const
{
  return impl_ && impl_->handle != nullptr;
}

void VendorLink::Close()
{
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->handle) {
    libusb_release_interface(impl_->handle, kStreamVendorItf);
    libusb_close(impl_->handle);
    impl_->handle = nullptr;
  }
  if (impl_->ctx) {
    libusb_exit(impl_->ctx);
    impl_->ctx = nullptr;
  }
}

bool VendorLink::Open(uint16_t vid, uint16_t pid, std::string &err)
{
  Close();
  int rc = libusb_init(&impl_->ctx);
  if (rc != 0) {
    err = std::string("libusb_init: ") + libusb_error_name(rc);
    return false;
  }
  impl_->handle = libusb_open_device_with_vid_pid(impl_->ctx, vid, pid);
  if (impl_->handle == nullptr) {
    err = "Channel Card vendor USB not found (VID/PID 0x4022 bulk BODY)";
    libusb_exit(impl_->ctx);
    impl_->ctx = nullptr;
    return false;
  }
#if defined(__linux__)
  (void)libusb_set_auto_detach_kernel_driver(impl_->handle, 1);
#endif
  rc = libusb_claim_interface(impl_->handle, kStreamVendorItf);
  if (rc != 0) {
    err = std::string("claim vendor itf: ") + libusb_error_name(rc);
    libusb_close(impl_->handle);
    impl_->handle = nullptr;
    libusb_exit(impl_->ctx);
    impl_->ctx = nullptr;
    return false;
  }
  return true;
}

bool VendorLink::Write(const void *data, int nbytes, unsigned timeout_ms,
                       std::string &err)
{
  if (data == nullptr || nbytes <= 0) {
    err = "empty write";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->handle == nullptr) {
    err = "vendor USB closed";
    return false;
  }
  int transferred = 0;
  int rc = libusb_bulk_transfer(impl_->handle, kStreamEpOut,
                                static_cast<unsigned char *>(
                                    const_cast<void *>(data)),
                                nbytes, &transferred, timeout_ms);
  if (rc != 0 || transferred != nbytes) {
    err = std::string("bulk OUT: ") + libusb_error_name(rc);
    return false;
  }
  return true;
}

} // namespace usb
} // namespace cardlink
