#include "cardlink/audio/sample_bulk.hpp"

#include "cardlink/usb/stream_proto.hpp"
#include "cardlink/usb/vendor_link.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace cardlink::audio {
namespace {

constexpr unsigned kPumpHz = 1000;

} // namespace

struct SampleBulkOut::Impl {
  cardlink::usb::VendorLink link;
  std::thread th;
  std::atomic<bool> run{false};
};

SampleBulkOut::SampleBulkOut() : impl_(std::make_unique<Impl>()) {}

void SampleBulkOut::BindMixer(SampleDryMixer &mixer) { mixer_ = &mixer; }

SampleBulkOut::~SampleBulkOut() { Stop(); }

bool SampleBulkOut::Running() const
{
  return impl_ && impl_->run.load(std::memory_order_acquire);
}

void SampleBulkOut::Stop()
{
  if (!impl_) {
    return;
  }
  impl_->run.store(false, std::memory_order_release);
  if (impl_->th.joinable()) {
    impl_->th.join();
  }
  impl_->link.Close();
}

bool SampleBulkOut::Start(std::string &err)
{
  Stop();
  if (!impl_->link.Open(cardlink::usb::kStreamVid, cardlink::usb::kStreamPid,
                        err)) {
    return false;
  }
  impl_->run.store(true, std::memory_order_release);
  impl_->th = std::thread([this]() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::vector<uint8_t> pkt(cardlink::usb::kStreamHdrSize +
                             cardlink::usb::kStreamBodyMetaSize +
                             cardlink::usb::kStreamNsampMax * 2u);
    std::array<int16_t, cardlink::usb::kStreamNsampMax> body{};
    while (impl_->run.load(std::memory_order_acquire)) {
      const auto now = clock::now();
      const double dt =
          std::chrono::duration<double>(now - last).count();
      last = now;
      double nframes = dt * static_cast<double>(kSampleRateHz);
      if (nframes > static_cast<double>(kSampleRateHz)) {
        nframes = static_cast<double>(kSampleRateHz);
      }
      mixer_->ConsumeOutputSamples(nframes);
      for (uint8_t v = 0; v < kSampleVoices; ++v) {
        const unsigned n = mixer_->WantBurst(v);
        if (n == 0) {
          continue;
        }
        bool sof = false;
        uint8_t session = 0;
        const unsigned got =
            mixer_->FillBurst(v, body.data(), n, sof, session);
        if (got == 0) {
          continue;
        }
        cardlink::usb::StreamHdr hdr{};
        hdr.magic0 = cardlink::usb::kStreamMagic0;
        hdr.magic1 = cardlink::usb::kStreamMagic1;
        hdr.type = cardlink::usb::kStreamTypeBody;
        hdr.nbytes = static_cast<uint16_t>(
            cardlink::usb::kStreamBodyMetaSize + got * 2u);
        cardlink::usb::StreamBodyMeta meta{};
        meta.voice = v;
        meta.session = session;
        meta.sof = sof ? 1u : 0u;
        meta.nsamp = static_cast<uint16_t>(got);
        std::memcpy(pkt.data(), &hdr, sizeof hdr);
        std::memcpy(pkt.data() + sizeof hdr, &meta, sizeof meta);
        std::memcpy(pkt.data() + sizeof hdr + sizeof meta, body.data(),
                    got * sizeof(int16_t));
        std::string werr;
        (void)impl_->link.Write(pkt.data(),
                                static_cast<int>(sizeof hdr + sizeof meta +
                                                 got * sizeof(int16_t)),
                                20, werr);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(1000000 / kPumpHz));
    }
  });
  return true;
}

} // namespace cardlink::audio
