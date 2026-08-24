#include "cardlink/audio/sample_bulk.hpp"

#include "cardlink/usb/stream_proto.hpp"
#include "cardlink/usb/vendor_link.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace cardlink::audio {
namespace {

constexpr unsigned kWriteTimeoutMs = 100;

struct PackedChunk {
  uint8_t voice = 0;
  unsigned nsamp = 0;
  bool sof = false;
};

int PackFrame(SampleDryMixer &mixer, uint8_t *dst, int max,
              PackedChunk *chunks, unsigned &nchunks,
              uint16_t pack_sequence)
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
  /* Maximum-size synchronous transfers plateau below the rate sustained by
   * pipelined ~2 KB transfers on the production macOS/hub/TinyUSB path.
   * Do not split a coalesced per-voice grant across many metas: serve the
   * hungriest eligible voices on successive RS-485 polls instead. */
  while (nuse > 1u &&
         nuse * kMinBurst > cardlink::usb::kStreamSubmitSamples) {
    --nuse;
  }
  while (nuse > 0 && cardlink::usb::PackMaxSamples(nuse) == 0) {
    --nuse;
  }
  if (nuse == 0) {
    return 0;
  }

  const unsigned wire_budget = cardlink::usb::PackMaxSamples(nuse);
  const unsigned sample_budget =
      std::min(wire_budget, cardlink::usb::kStreamSubmitSamples);
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
  hdr.pad = pack_sequence;
  std::memcpy(dst, &hdr, sizeof hdr);
  return static_cast<int>(used);
}

} // namespace

struct SampleBulkOut::Impl {
  struct RefillStatus {
    uint8_t mask = 0u;
    uint8_t best = 0xFFu;
    std::array<uint16_t, kSampleVoices> free_samples{};
    uint16_t last_pack_sequence = 0xFFFFu;
  };

  struct StatusRequest {
    RefillStatus status{};
    std::array<uint16_t, kSampleVoices> unreflected{};
  };

  struct OutstandingPack {
    uint16_t sequence = 0u;
    std::array<uint16_t, kSampleVoices> samples{};
  };

  cardlink::usb::VendorLink link;
  std::thread th;
  std::atomic<bool> run{false};
  std::mutex status_mu;
  std::mutex mixer_mu;
  std::condition_variable status_cv;
  std::deque<RefillStatus> status_queue;
  std::deque<OutstandingPack> outstanding;
  uint16_t next_pack_sequence = 0u;
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
  impl_->status_cv.notify_all();
  if (impl_->th.joinable()) {
    impl_->th.join();
  }
  impl_->link.Close();
}

void SampleBulkOut::SubmitStatus(
    uint8_t mask, uint8_t best,
    const std::array<uint16_t, kSampleVoices> &free_samples,
    uint16_t last_pack_sequence)
{
  if (!impl_ || !impl_->run.load(std::memory_order_acquire)) {
    return;
  }
  Impl::RefillStatus status{};
  status.mask = mask;
  status.best = best;
  status.free_samples = free_samples;
  status.last_pack_sequence = last_pack_sequence;
  {
    std::lock_guard<std::mutex> lock(impl_->status_mu);
    /* Preserve every sequential RS-485 snapshot. Each one is a distinct
     * refill opportunity and acknowledges a precise USB PACK frontier. */
    impl_->status_queue.push_back(status);
  }
  impl_->status_cv.notify_one();
}

bool SampleBulkOut::Start(std::string &err)
{
  Stop();
  if (!impl_->link.Open(cardlink::usb::kStreamVid, cardlink::usb::kStreamPid,
                        err)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->status_mu);
    impl_->status_queue.clear();
    impl_->outstanding.clear();
  }
  impl_->run.store(true, std::memory_order_release);
  impl_->th = std::thread([this]() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    std::vector<uint8_t> pkt(cardlink::usb::kStreamFrameMax);
    std::array<PackedChunk, kSampleVoices> chunks{};
    while (impl_->run.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(impl_->status_mu);
      impl_->status_cv.wait(lock, [this]() {
        return !impl_->run.load(std::memory_order_acquire) ||
               !impl_->status_queue.empty();
      });
      if (!impl_->run.load(std::memory_order_acquire)) {
        break;
      }
      Impl::StatusRequest request{};
      request.status = impl_->status_queue.front();
      impl_->status_queue.pop_front();
      for (auto it = impl_->outstanding.begin();
           it != impl_->outstanding.end(); ++it) {
        if (it->sequence == request.status.last_pack_sequence) {
          impl_->outstanding.erase(impl_->outstanding.begin(), std::next(it));
          break;
        }
      }
      for (const auto &pack : impl_->outstanding) {
        for (unsigned v = 0u; v < kSampleVoices; ++v) {
          const unsigned total = static_cast<unsigned>(request.unreflected[v]) +
                                 pack.samples[v];
          request.unreflected[v] = static_cast<uint16_t>(
              total > 0xFFFFu ? 0xFFFFu : total);
        }
      }

      const auto now = clock::now();
      const double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      double nframes = dt * static_cast<double>(kSampleRateHz);
      if (nframes > static_cast<double>(kSampleRateHz)) {
        nframes = static_cast<double>(kSampleRateHz);
      }
      unsigned nchunks = 0;
      const uint16_t pack_sequence = impl_->next_pack_sequence++;
      int nbytes = 0;
      {
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        mixer_->ApplyVoiceStatus(request.status.mask, request.status.best,
                                 request.status.free_samples.data(),
                                 request.unreflected.data());
        mixer_->ConsumeOutputSamples(nframes);
        nbytes = PackFrame(*mixer_, pkt.data(),
                           static_cast<int>(cardlink::usb::kStreamFrameMax),
                           chunks.data(), nchunks, pack_sequence);
      }
      if (nbytes <= 0 || nchunks == 0) {
        lock.unlock();
        continue;
      }
      Impl::OutstandingPack outstanding{};
      outstanding.sequence = pack_sequence;
      for (unsigned i = 0; i < nchunks; ++i) {
        outstanding.samples[chunks[i].voice] =
            static_cast<uint16_t>(chunks[i].nsamp);
      }
      impl_->outstanding.push_back(outstanding);
      lock.unlock();

      std::string werr;
      const auto submitted_chunks = chunks;
      const bool submitted = impl_->link.SubmitWrite(
          pkt.data(), nbytes, kWriteTimeoutMs,
          [this, submitted_chunks, nchunks, pack_sequence](bool wrote) {
            if (!wrote) {
              {
                std::lock_guard<std::mutex> done_lock(impl_->status_mu);
                for (auto it = impl_->outstanding.begin();
                     it != impl_->outstanding.end(); ++it) {
                  if (it->sequence == pack_sequence) {
                    impl_->outstanding.erase(it);
                    break;
                  }
                }
              }
              std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
              for (unsigned i = 0; i < nchunks; ++i) {
                mixer_->AbortBurst(submitted_chunks[i].voice,
                                   submitted_chunks[i].nsamp,
                                   submitted_chunks[i].sof);
              }
            } else {
              for (unsigned i = 0; i < nchunks; ++i) {
                mixer_->CommitBurst(submitted_chunks[i].voice,
                                    submitted_chunks[i].nsamp,
                                    submitted_chunks[i].sof);
              }
            }
          },
          werr);
      if (!submitted) {
        {
          std::lock_guard<std::mutex> done_lock(impl_->status_mu);
          for (auto it = impl_->outstanding.begin();
               it != impl_->outstanding.end(); ++it) {
            if (it->sequence == pack_sequence) {
              impl_->outstanding.erase(it);
              break;
            }
          }
        }
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        for (unsigned i = 0; i < nchunks; ++i) {
          mixer_->AbortBurst(chunks[i].voice, chunks[i].nsamp,
                             chunks[i].sof);
        }
      }
    }
  });
  return true;
}

} // namespace cardlink::audio
