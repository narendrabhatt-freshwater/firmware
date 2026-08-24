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
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cardlink {
namespace usb {

struct VendorLink::Impl {
  struct AsyncOut {
    Impl *owner = nullptr;
    libusb_transfer *transfer = nullptr;
    std::vector<unsigned char> data;
    std::function<void(bool)> completion;
  };

  libusb_context *ctx = nullptr;
  libusb_device_handle *handle = nullptr;
  std::mutex mu;
  std::condition_variable cv;
  std::unordered_set<AsyncOut *> async_out;
  std::thread event_thread;
  std::atomic<bool> events_run{false};
  bool closing = false;
  static void LIBUSB_CALL AsyncOutComplete(libusb_transfer *transfer);
};

void LIBUSB_CALL VendorLink::Impl::AsyncOutComplete(libusb_transfer *transfer)
{
  auto *out = static_cast<VendorLink::Impl::AsyncOut *>(transfer->user_data);
  auto *owner = out->owner;
  const bool ok = transfer->status == LIBUSB_TRANSFER_COMPLETED &&
                  transfer->actual_length == transfer->length;
  std::function<void(bool)> completion = std::move(out->completion);
  {
    std::lock_guard<std::mutex> lock(owner->mu);
    owner->async_out.erase(out);
  }
  libusb_free_transfer(transfer);
  delete out;
  owner->cv.notify_all();
  if (completion) {
    completion(ok);
  }
}

VendorLink::VendorLink() : impl_(std::make_unique<Impl>()) {}

VendorLink::~VendorLink() { Close(); }

void VendorLink::Close()
{
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->closing = true;
    /* Keep ownership locked while issuing cancellation. Otherwise an event
     * callback can erase/free a transfer after its pointer is copied but
     * before libusb_cancel_transfer() uses it. Cancellation is asynchronous;
     * callbacks drain below after this lock is released. */
    for (auto *out : impl_->async_out) {
      (void)libusb_cancel_transfer(out->transfer);
    }
  }
  if (impl_->ctx) {
    std::unique_lock<std::mutex> lock(impl_->mu);
    impl_->cv.wait(lock, [this] { return impl_->async_out.empty(); });
  }
  impl_->events_run.store(false, std::memory_order_release);
  if (impl_->event_thread.joinable()) {
    impl_->event_thread.join();
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
  impl_->closing = false;
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
  impl_->events_run.store(true, std::memory_order_release);
  impl_->event_thread = std::thread([this] {
    while (impl_->events_run.load(std::memory_order_acquire)) {
      timeval tv{0, 10000};
      (void)libusb_handle_events_timeout(impl_->ctx, &tv);
    }
  });
  return true;
}

bool VendorLink::SubmitWrite(const void *data, int nbytes,
                             unsigned timeout_ms,
                             std::function<void(bool)> completion,
                             std::string &err)
{
  if (data == nullptr || nbytes <= 0) {
    err = "empty write";
    return false;
  }
  auto *out = new Impl::AsyncOut;
  out->owner = impl_.get();
  out->data.resize(static_cast<size_t>(nbytes));
  std::memcpy(out->data.data(), data, static_cast<size_t>(nbytes));
  out->completion = std::move(completion);
  out->transfer = libusb_alloc_transfer(0);
  if (out->transfer == nullptr) {
    delete out;
    err = "allocate bulk OUT transfer";
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->handle == nullptr || impl_->closing) {
    libusb_free_transfer(out->transfer);
    delete out;
    err = "vendor USB closed";
    return false;
  }
  libusb_fill_bulk_transfer(out->transfer, impl_->handle, kStreamEpOut,
                            out->data.data(), nbytes, Impl::AsyncOutComplete, out,
                            timeout_ms);
  impl_->async_out.insert(out);
  const int rc = libusb_submit_transfer(out->transfer);
  if (rc != 0) {
    impl_->async_out.erase(out);
    libusb_free_transfer(out->transfer);
    delete out;
    err = std::string("submit bulk OUT: ") + libusb_error_name(rc);
    return false;
  }
  return true;
}

} // namespace usb
} // namespace cardlink
