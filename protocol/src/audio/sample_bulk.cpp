#include "cardlink/audio/sample_bulk.hpp"

#include "cardlink/usb/stream_proto.hpp"

#include <RtAudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cardlink::audio {
namespace {

constexpr unsigned kMaxOutstandingPacks = 3u;
constexpr unsigned kRetryAfterUnackedVq = 4u;
constexpr auto kRetryMinAge = std::chrono::milliseconds(30);
constexpr double kPackHorizonMs = 8.0;

struct PackedChunk {
  uint8_t voice = 0;
  uint8_t session = 0;
  uint16_t wave_id = 0xFFFFu;
  unsigned nsamp = 0;
  bool sof = false;
};

int PackFrame(SampleDryMixer &mixer, uint8_t *dst, int max,
              PackedChunk *chunks, unsigned &nchunks,
              uint16_t pack_sequence)
{
  nchunks = 0;
  if (dst == nullptr ||
      max < static_cast<int>(cardlink::usb::kStreamHdrSize +
                             cardlink::usb::kStreamBodyMetaSize + 2u)) {
    return 0;
  }
  const unsigned cap = static_cast<unsigned>(max);
  uint8_t order[kSampleVoices];
  const unsigned nwant = mixer.WantingVoices(order);
  if (nwant == 0u) {
    return 0;
  }
  unsigned nuse = nwant;
  while (nuse > 0u && cardlink::usb::PackMaxSamples(nuse) == 0u) {
    --nuse;
  }
  if (nuse == 0u) {
    return 0;
  }

  const unsigned wire_budget = cardlink::usb::PackMaxSamples(nuse);
  const unsigned predicted_budget = mixer.SourceDemandSamples(kPackHorizonMs);
  const unsigned sample_budget = std::min(wire_budget, predicted_budget);
  if (sample_budget == 0u) {
    return 0;
  }
  unsigned used = cardlink::usb::kStreamHdrSize;
  std::array<int16_t, cardlink::usb::kStreamNsampMax> body{};
  unsigned grants[kSampleVoices]{};
  (void)mixer.AllocateBursts(order, nuse, sample_budget, grants);

  for (unsigned i = 0u; i < nuse; ++i) {
    const uint8_t voice = order[i];
    unsigned give = grants[i];
    if (give == 0u) {
      continue;
    }
    const unsigned room = cap - used;
    if (room < cardlink::usb::kStreamBodyMetaSize + 2u) {
      break;
    }
    const unsigned max_samples =
        (room - cardlink::usb::kStreamBodyMetaSize) / 2u;
    if (give > max_samples) {
      give = max_samples;
    }
    bool sof = false;
    uint8_t session = 0u;
    /* Capture before FillBurst: a one-shot may retire itself after producing
     * its final valid chunk, at which point LiveWave intentionally goes idle. */
    const uint16_t wave_id = mixer.LiveWave(voice);
    const unsigned got =
        mixer.FillBurst(voice, body.data(), give, sof, session);
    if (got == 0u) {
      continue;
    }
    cardlink::usb::StreamBodyMeta meta{};
    meta.voice = voice;
    meta.session = session;
    meta.sof = sof ? 1u : 0u;
    meta.nsamp = static_cast<uint16_t>(got);
    meta.wave_id = wave_id;
    std::memcpy(dst + used, &meta, sizeof meta);
    std::memcpy(dst + used + sizeof meta, body.data(),
                got * sizeof(int16_t));
    used += sizeof meta + got * sizeof(int16_t);
    chunks[nchunks++] = PackedChunk{voice, session, wave_id, got, sof};
  }
  if (nchunks == 0u) {
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

bool NameMatches(const std::string &name)
{
  return name.find("Channel Card BODY") != std::string::npos ||
         name.find("Channel Card Audio") != std::string::npos ||
         name.find("Channel Card") != std::string::npos;
}

} // namespace

struct SampleBulkOut::Impl {
  using clock = std::chrono::steady_clock;

  struct RefillStatus {
    uint8_t mask = 0u;
    uint8_t best = 0xFFu;
    std::array<uint16_t, kSampleVoices> free_samples{};
    uint16_t last_pack_sequence = 0xFFFFu;
  };

  struct OutstandingPack {
    uint16_t sequence = 0u;
    std::vector<uint8_t> wire;
    std::array<PackedChunk, kSampleVoices> chunks{};
    unsigned nchunks = 0u;
    std::array<uint16_t, kSampleVoices> samples{};
    bool rendered = false;
    bool queued = false;
    unsigned unacked_vq = 0u;
    clock::time_point rendered_at{};
  };

  RtAudio dac;
  std::thread th;
  std::atomic<bool> run{false};
  std::atomic<uint32_t> xruns{0u};
  std::atomic<uint64_t> render_frames{0u};
  std::mutex mu;
  std::mutex mixer_mu;
  std::condition_variable cv;
  std::deque<RefillStatus> status_queue;
  std::deque<std::shared_ptr<OutstandingPack>> outstanding;
  std::deque<std::shared_ptr<OutstandingPack>> render_queue;
  std::shared_ptr<OutstandingPack> render_pack;
  size_t render_pack_offset = 0u;
  uint16_t next_pack_sequence = 0u;

  void QueueForRender(const std::shared_ptr<OutstandingPack> &pack,
                      bool priority = false)
  {
    if (!pack || pack->queued) {
      return;
    }
    pack->queued = true;
    pack->rendered = false;
    pack->unacked_vq = 0u;
    if (priority) {
      render_queue.push_front(pack);
    } else {
      render_queue.push_back(pack);
    }
  }

  void Render(void *output, unsigned nframes, RtAudioStreamStatus status)
  {
    render_frames.fetch_add(nframes, std::memory_order_relaxed);
    if (status != 0u) {
      xruns.fetch_add(1u, std::memory_order_relaxed);
    }
    uint8_t *dst = static_cast<uint8_t *>(output);
    size_t remaining = static_cast<size_t>(nframes) *
                       cardlink::usb::kStreamUacChannels *
                       cardlink::usb::kStreamUacSampleBytes;
    if (dst == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu);
    while (remaining != 0u) {
      if (!render_pack && !render_queue.empty()) {
        render_pack = render_queue.front();
        render_queue.pop_front();
        render_pack_offset = 0u;
      }
      if (!render_pack) {
        std::memset(dst, 0, remaining);
        break;
      }
      const size_t take = std::min(
          remaining, render_pack->wire.size() - render_pack_offset);
      std::memcpy(dst, render_pack->wire.data() + render_pack_offset, take);
      dst += take;
      remaining -= take;
      render_pack_offset += take;
      if (render_pack_offset == render_pack->wire.size()) {
        render_pack->rendered = true;
        render_pack->queued = false;
        render_pack->rendered_at = clock::now();
        render_pack.reset();
        render_pack_offset = 0u;
        cv.notify_one();
      }
    }
  }

  static int AudioCallback(void *output, void *, unsigned nframes, double,
                           RtAudioStreamStatus status, void *user)
  {
    auto *impl = static_cast<Impl *>(user);
    if (impl != nullptr) {
      impl->Render(output, nframes, status);
    }
    return 0;
  }
};

SampleBulkOut::SampleBulkOut() : impl_(std::make_unique<Impl>()) {}
SampleBulkOut::~SampleBulkOut() { Stop(); }
void SampleBulkOut::BindMixer(SampleDryMixer &mixer) { mixer_ = &mixer; }

bool SampleBulkOut::Running() const
{
  return impl_ && impl_->run.load(std::memory_order_acquire) &&
         impl_->dac.isStreamRunning();
}

uint32_t SampleBulkOut::XrunCount() const
{
  return impl_ ? impl_->xruns.load(std::memory_order_relaxed) : 0u;
}

uint64_t SampleBulkOut::RenderFrameCount() const
{
  return impl_ ? impl_->render_frames.load(std::memory_order_relaxed) : 0u;
}

void SampleBulkOut::Stop()
{
  if (!impl_) {
    return;
  }
  impl_->run.store(false, std::memory_order_release);
  impl_->cv.notify_all();
  if (impl_->th.joinable()) {
    impl_->th.join();
  }
  if (impl_->dac.isStreamRunning()) {
    impl_->dac.stopStream();
  }
  if (impl_->dac.isStreamOpen()) {
    impl_->dac.closeStream();
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
  for (auto pit = impl_->outstanding.rbegin();
       pit != impl_->outstanding.rend(); ++pit) {
    const auto &pack = *pit;
    for (unsigned i = pack->nchunks; i > 0u; --i) {
      const auto &chunk = pack->chunks[i - 1u];
      mixer_->AbortBurst(chunk.voice, chunk.session, chunk.wave_id,
                         chunk.nsamp, chunk.sof);
    }
  }
  impl_->status_queue.clear();
  impl_->outstanding.clear();
  impl_->render_queue.clear();
  impl_->render_pack.reset();
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
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->status_queue.push_back(status);
  }
  impl_->cv.notify_one();
}

bool SampleBulkOut::Start(std::string &err)
{
  Stop();
  if (!impl_) {
    err = "no RtAudio";
    return false;
  }
  /* CoreAudio may call Render() before startStream() returns. Establish an
   * empty generation first so a restart can only emit zeroes until a fresh
   * vq authorizes new PACKs; never expose queues from the previous stream. */
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->status_queue.clear();
    impl_->outstanding.clear();
    impl_->render_queue.clear();
    impl_->render_pack.reset();
    impl_->render_pack_offset = 0u;
    impl_->next_pack_sequence = 0u;
  }
  impl_->xruns.store(0u, std::memory_order_relaxed);
  impl_->render_frames.store(0u, std::memory_order_relaxed);
  unsigned device = 0u;
  bool found = false;
  std::string seen;
  for (unsigned id : impl_->dac.getDeviceIds()) {
    const RtAudio::DeviceInfo info = impl_->dac.getDeviceInfo(id);
    if (!seen.empty()) {
      seen += ", ";
    }
    seen += info.name + "[" + std::to_string(info.outputChannels) + "ch]";
    if (info.outputChannels >= cardlink::usb::kStreamUacChannels &&
        NameMatches(info.name)) {
      device = id;
      found = true;
      break;
    }
  }
  if (!found) {
    err = "no 10-channel Channel Card UAC output";
    if (!seen.empty()) {
      err += " (seen: " + seen + ")";
    }
    return false;
  }

  RtAudio::StreamParameters params;
  params.deviceId = device;
  params.nChannels = cardlink::usb::kStreamUacChannels;
  params.firstChannel = 0u;
  unsigned buffer_frames = 102u;
  RtAudioErrorType rc = impl_->dac.openStream(
      &params, nullptr, RTAUDIO_SINT16, cardlink::usb::kStreamUacRateHz,
      &buffer_frames,
      &Impl::AudioCallback, impl_.get());
  if (rc != RTAUDIO_NO_ERROR) {
    err = impl_->dac.getErrorText();
    return false;
  }
  rc = impl_->dac.startStream();
  if (rc != RTAUDIO_NO_ERROR) {
    err = impl_->dac.getErrorText();
    impl_->dac.closeStream();
    return false;
  }

  impl_->run.store(true, std::memory_order_release);
  impl_->th = std::thread([this]() {
    using clock = Impl::clock;
    auto last = clock::now();
    while (impl_->run.load(std::memory_order_acquire)) {
      Impl::RefillStatus status{};
      std::vector<std::shared_ptr<Impl::OutstandingPack>> acked;
      std::unique_lock<std::mutex> lock(impl_->mu);
      impl_->cv.wait(lock, [this]() {
        return !impl_->run.load(std::memory_order_acquire) ||
               !impl_->status_queue.empty();
      });
      if (!impl_->run.load(std::memory_order_acquire)) {
        break;
      }
      status = impl_->status_queue.front();
      impl_->status_queue.pop_front();

      if (status.last_pack_sequence != 0xFFFFu) {
        auto ack_it = std::find_if(
            impl_->outstanding.begin(), impl_->outstanding.end(),
            [&status](const auto &pack) {
              return pack->sequence == status.last_pack_sequence;
            });
        if (ack_it != impl_->outstanding.end()) {
          for (auto it = impl_->outstanding.begin(); it != std::next(ack_it);
               ++it) {
            acked.push_back(*it);
          }
          impl_->outstanding.erase(impl_->outstanding.begin(),
                                   std::next(ack_it));
          for (const auto &pack : acked) {
            impl_->render_queue.erase(
                std::remove(impl_->render_queue.begin(),
                            impl_->render_queue.end(), pack),
                impl_->render_queue.end());
            if (impl_->render_pack != pack) {
              pack->queued = false;
            }
          }
        }
      }
      lock.unlock();
      bool replaced_note = false;
      {
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        /* Apply note commands before ACK accounting. An old PACK may be
         * transport-ACKed after its voice has already been replaced. */
        mixer_->ConsumeOutputSamples(0.0);
        for (const auto &pack : acked) {
          for (unsigned i = 0u; i < pack->nchunks; ++i) {
            const auto &chunk = pack->chunks[i];
            if (!mixer_->BurstIsCurrent(chunk.voice, chunk.session,
                                        chunk.wave_id)) {
              replaced_note = true;
            }
            mixer_->CommitBurst(chunk.voice, chunk.session, chunk.wave_id,
                                chunk.nsamp, chunk.sof);
          }
        }
      }
      lock.lock();

      /* A replacement invalidates every old-note chunk. Anything already in
       * the active callback must finish so the card can parse its CRC and
       * sequence. Everything still waiting behind it is a replaceable suffix:
       * rewind that sequence and rebuild it from the urgent new session. */
      {
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        mixer_->ConsumeOutputSamples(0.0);
        bool stale_outstanding = false;
        for (const auto &pack : impl_->outstanding) {
          for (unsigned i = 0u; i < pack->nchunks; ++i) {
            const auto &chunk = pack->chunks[i];
            if (!mixer_->BurstIsCurrent(chunk.voice, chunk.session,
                                        chunk.wave_id)) {
              stale_outstanding = true;
              break;
            }
          }
          if (stale_outstanding) {
            break;
          }
        }
        if (replaced_note || stale_outstanding) {
          auto cancel_it = std::find_if(
              impl_->outstanding.begin(), impl_->outstanding.end(),
              [this](const auto &pack) {
                return !pack->rendered && impl_->render_pack != pack &&
                       std::find(impl_->render_queue.begin(),
                                 impl_->render_queue.end(), pack) !=
                           impl_->render_queue.end();
              });
          if (cancel_it != impl_->outstanding.end()) {
            impl_->next_pack_sequence = (*cancel_it)->sequence;
            for (auto it = impl_->outstanding.end(); it != cancel_it;) {
              --it;
              const auto &pack = *it;
              impl_->render_queue.erase(
                  std::remove(impl_->render_queue.begin(),
                              impl_->render_queue.end(), pack),
                  impl_->render_queue.end());
              pack->queued = false;
              for (unsigned i = pack->nchunks; i > 0u; --i) {
                const auto &chunk = pack->chunks[i - 1u];
                mixer_->AbortBurst(chunk.voice, chunk.session, chunk.wave_id,
                                   chunk.nsamp, chunk.sof);
              }
            }
            impl_->outstanding.erase(cancel_it, impl_->outstanding.end());
          }
        }
      }

      /* Keep the UAC pipe full while its application ACK follows one vq
       * behind. Every outstanding sample is subtracted from this status's
       * exact credit below. Four later vq snapshots plus 30 ms without an ACK
       * are strong evidence of ISO loss; that fresh vq authorizes a priority
       * retry of the oldest missing sequence. */
      const auto now = clock::now();
      bool retried = false;
      if (!impl_->outstanding.empty()) {
        const auto &pack = impl_->outstanding.front();
        if (pack->rendered && !pack->queued) {
          ++pack->unacked_vq;
        }
        if (pack->rendered && !pack->queued &&
            pack->unacked_vq >= kRetryAfterUnackedVq &&
            now - pack->rendered_at >= kRetryMinAge) {
          impl_->QueueForRender(pack, true);
          retried = true;
        }
      }
      /* A single fresh vq authorizes exactly one action: either the retry
       * above or one new ordered PACK below. Bound the pipeline so a stopped
       * endpoint cannot accumulate reservations indefinitely. */
      if (retried || impl_->outstanding.size() >= kMaxOutstandingPacks) {
        lock.unlock();
        continue;
      }

      /* Do file-cursor work and PACK construction away from the CoreAudio
       * mutex. The real-time callback only needs that mutex for short queue
       * operations and must never wait behind ~4800-sample allocation/copy. */
      lock.unlock();
      std::array<uint16_t, kSampleVoices> unreflected{};
      for (const auto &pending : impl_->outstanding) {
        for (unsigned voice = 0u; voice < kSampleVoices; ++voice) {
          const unsigned total =
              static_cast<unsigned>(unreflected[voice]) +
              pending->samples[voice];
          unreflected[voice] = static_cast<uint16_t>(
              std::min(total, static_cast<unsigned>(0xFFFFu)));
        }
      }
      const double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      double nframes = std::min(
          dt * static_cast<double>(kSampleRateHz),
          static_cast<double>(kSampleRateHz));
      const uint16_t sequence = impl_->next_pack_sequence;
      auto pack = std::make_shared<Impl::OutstandingPack>();
      pack->sequence = sequence;
      pack->wire.resize(cardlink::usb::kStreamUacWindowBytes);
      unsigned nchunks = 0u;
      int nbytes = 0;
      {
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        /* Predict forward to the timestamp of this vq, then reconcile to the
         * card's exact snapshot. Reversing these calls subtracts the same
         * playback interval twice and can over-credit the refill. */
        mixer_->ConsumeOutputSamples(nframes);
        mixer_->ApplyVoiceStatus(status.mask, status.best,
                                 status.free_samples.data(),
                                 unreflected.data());
        mixer_->ConsumeOutputSamples(0.0);
        nbytes = PackFrame(*mixer_, pack->wire.data(),
                           static_cast<int>(cardlink::usb::kStreamFrameMax),
                           pack->chunks.data(), nchunks, sequence);
      }
      if (nbytes <= 0 || nchunks == 0u) {
        continue;
      }
      const uint32_t crc = cardlink::usb::StreamCrc32(
          pack->wire.data(), static_cast<size_t>(nbytes));
      std::memcpy(pack->wire.data() + nbytes, &crc, sizeof crc);
      nbytes += static_cast<int>(sizeof crc);
      impl_->next_pack_sequence =
          cardlink::usb::NextPackSequence(impl_->next_pack_sequence);
      pack->wire.resize(static_cast<size_t>(nbytes));
      pack->nchunks = nchunks;
      for (unsigned i = 0u; i < nchunks; ++i) {
        pack->samples[pack->chunks[i].voice] =
            static_cast<uint16_t>(pack->chunks[i].nsamp);
      }
      lock.lock();
      if (!impl_->run.load(std::memory_order_acquire)) {
        lock.unlock();
        std::lock_guard<std::mutex> mixer_lock(impl_->mixer_mu);
        for (unsigned i = pack->nchunks; i > 0u; --i) {
          const auto &chunk = pack->chunks[i - 1u];
          mixer_->AbortBurst(chunk.voice, chunk.session, chunk.wave_id,
                             chunk.nsamp, chunk.sof);
        }
        break;
      }
      impl_->outstanding.push_back(pack);
      impl_->QueueForRender(pack);
      lock.unlock();
    }
  });
  return true;
}

} // namespace cardlink::audio
