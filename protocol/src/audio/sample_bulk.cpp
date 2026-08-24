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
constexpr unsigned kWriteTimeoutMs = 100;
/* One transfer per vq-paced tick. Extra packs would lack fresh permission. */
constexpr unsigned kMaxPacksPerTick = 1;

struct PackedChunk {
  uint8_t voice = 0;
  unsigned nsamp = 0;
  bool sof = false;
};

int PackFrame(SampleDryMixer &mixer, uint8_t *dst, int max,
              PackedChunk *chunks, unsigned &nchunks)
{
  nchunks = 0;
  if (dst == nullptr || max < static_cast<int>(cardlink::usb::kStreamHdrSize +
                                               cardlink::usb::kStreamBodyMetaSize +
                                               2u)) {
    return 0;
  }
  const unsigned cap = static_cast<unsigned>(max);

  uint8_t order[kSampleVoices];
  const unsigned nwant = mixer.WantingVoices(order);
  if (nwant == 0) {
    return 0;
  }

  unsigned nuse = nwant;
  while (nuse > 0 && cardlink::usb::PackMaxSamples(nuse) == 0) {
    --nuse;
  }
  if (nuse == 0) {
    return 0;
  }

  const unsigned sample_budget = cardlink::usb::PackMaxSamples(nuse);
  unsigned used = cardlink::usb::kStreamHdrSize;
  std::array<int16_t, cardlink::usb::kStreamNsampMax> body{};
  unsigned grants[kSampleVoices]{};
  (void)mixer.AllocateBursts(order, nuse, sample_budget, grants);

  for (unsigned i = 0; i < nuse; ++i) {
    const uint8_t v = order[i];
    unsigned give = grants[i];
    if (give == 0u) {
      continue;
    }
    const unsigned room = cap - used;
    if (room < cardlink::usb::kStreamBodyMetaSize + 2u) {
      break;
    }
    unsigned max_s = (room - cardlink::usb::kStreamBodyMetaSize) / 2u;
    if (give > max_s) {
      give = max_s;
    }
    if (give == 0) {
      continue;
    }
    bool sof = false;
    uint8_t session = 0;
    const unsigned got = mixer.FillBurst(v, body.data(), give, sof, session);
    if (got == 0) {
      continue;
    }
    cardlink::usb::StreamBodyMeta meta{};
    meta.voice = v;
    meta.session = session;
    meta.sof = sof ? 1u : 0u;
    meta.nsamp = static_cast<uint16_t>(got);
    std::memcpy(dst + used, &meta, sizeof meta);
    std::memcpy(dst + used + sizeof meta, body.data(),
                got * sizeof(int16_t));
    used += sizeof meta + got * sizeof(int16_t);
    chunks[nchunks].voice = v;
    chunks[nchunks].nsamp = got;
    chunks[nchunks].sof = sof;
    ++nchunks;
  }

  if (nchunks == 0) {
    return 0;
  }

  cardlink::usb::StreamHdr hdr{};
  hdr.magic0 = cardlink::usb::kStreamMagic0;
  hdr.magic1 = cardlink::usb::kStreamMagic1;
  hdr.type = cardlink::usb::kStreamTypePack;
  hdr.nbytes = static_cast<uint16_t>(used - cardlink::usb::kStreamHdrSize);
  std::memcpy(dst, &hdr, sizeof hdr);
  return static_cast<int>(used);
}

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
    auto next = last;
    std::vector<uint8_t> pkt(cardlink::usb::kStreamFrameMax);
    std::array<PackedChunk, kSampleVoices> chunks{};
    while (impl_->run.load(std::memory_order_acquire)) {
      const auto now = clock::now();
      const double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      double nframes = dt * static_cast<double>(kSampleRateHz);
      if (nframes > static_cast<double>(kSampleRateHz)) {
        nframes = static_cast<double>(kSampleRateHz);
      }
      mixer_->ConsumeOutputSamples(nframes);

      unsigned sends = 0;
      bool usb_ok = true;
      while (usb_ok && sends < kMaxPacksPerTick) {
        unsigned nchunks = 0;
        const int nbytes =
            PackFrame(*mixer_, pkt.data(),
                      static_cast<int>(cardlink::usb::kStreamFrameMax),
                      chunks.data(), nchunks);
        if (nbytes <= 0 || nchunks == 0) {
          break;
        }
        std::string werr;
        if (!impl_->link.Write(pkt.data(), nbytes, kWriteTimeoutMs, werr)) {
          for (unsigned i = 0; i < nchunks; ++i) {
            mixer_->AbortBurst(chunks[i].voice, chunks[i].nsamp,
                               chunks[i].sof);
          }
          usb_ok = false;
          break;
        }
        ++sends;
      }

      next += std::chrono::microseconds(1000000 / kPumpHz);
      const auto wake = clock::now();
      if (wake < next) {
        std::this_thread::sleep_until(next);
      } else {
        next = wake;
      }
    }
  });
  return true;
}

} // namespace cardlink::audio
