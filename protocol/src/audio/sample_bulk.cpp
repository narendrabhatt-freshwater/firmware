/* Direct tagged BODY samples over the class-compliant 10ch UAC2 output. */
#include "cardlink/audio/sample_bulk.hpp"

#include "cardlink/usb/stream_proto.hpp"

#include <RtAudio.h>

#include <algorithm>
#include <array>
#include <atomic>

namespace cardlink::audio {
namespace {

int16_t EncodeTag(uint8_t voice, uint8_t session, bool sof)
{
  const uint16_t tag = cardlink::usb::kStreamTagBase |
      (static_cast<uint16_t>(session) <<
       cardlink::usb::kStreamTagSessionShift) |
      (sof ? cardlink::usb::kStreamTagSof : 0u) |
      (static_cast<uint16_t>(voice) & cardlink::usb::kStreamTagVoiceMask);
  return static_cast<int16_t>(tag);
}

bool NameMatches(const std::string &name)
{
  return name.find("Channel Card BODY") != std::string::npos ||
         name.find("Channel Card Audio") != std::string::npos ||
         name.find("Channel Card") != std::string::npos;
}

} // namespace

struct SampleBulkOut::Impl {
  RtAudio dac;
  std::atomic<uint32_t> xruns{0u};
  std::atomic<uint64_t> render_frames{0u};
  std::atomic<bool> running{false};
  std::atomic<unsigned> urgent_voices{0u};
  std::atomic<double> urgent_demand{0.0};
  std::atomic<double> urgent_attack_ms{0.0};
  std::atomic<double> urgent_quantum_ms{0.0};
  std::array<int16_t, cardlink::usb::kStreamUacPacketWords> packet{};
  unsigned packet_words_used = cardlink::usb::kStreamUacPacketWords;
};

SampleBulkOut::SampleBulkOut() : impl_(std::make_unique<Impl>()) {}
SampleBulkOut::~SampleBulkOut() { Stop(); }
void SampleBulkOut::BindMixer(SampleDryMixer &mixer) { mixer_ = &mixer; }

bool SampleBulkOut::Start(std::string &err)
{
  Stop();
  if (!impl_) {
    err = "no RtAudio";
    return false;
  }
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

  impl_->xruns.store(0u, std::memory_order_relaxed);
  impl_->render_frames.store(0u, std::memory_order_relaxed);
  mixer_->DrainPendingCommands();
  impl_->packet_words_used = cardlink::usb::kStreamUacPacketWords;
  RtAudio::StreamParameters params;
  params.deviceId = device;
  params.nChannels = cardlink::usb::kStreamUacChannels;
  params.firstChannel = 0u;
  /* Keep producer latency to one physical USB millisecond. At C6 the local
   * attack reaches BODY in about 2.5 ms, so a two-millisecond CoreAudio
   * callback plus command acknowledgement can expose the boundary. */
  unsigned buffer_frames = cardlink::usb::kStreamUacFramesPerMs;
  RtAudioErrorType rc = impl_->dac.openStream(
      &params, nullptr, RTAUDIO_SINT16, cardlink::usb::kStreamUacRateHz,
      &buffer_frames,
      [](void *output, void *, unsigned nframes, double,
         RtAudioStreamStatus status, void *user) -> int {
        auto *self = static_cast<SampleBulkOut *>(user);
        auto *dst = static_cast<int16_t *>(output);
        if (self == nullptr || dst == nullptr) {
          return 0;
        }
        auto &mixer = self->Mixer();
        auto &state = *self->impl_;
        state.render_frames.fetch_add(nframes, std::memory_order_relaxed);
        if (status != 0u) {
          state.xruns.fetch_add(1u, std::memory_order_relaxed);
        }
        mixer.ConsumeOutputSamples(
            static_cast<double>(nframes) *
            static_cast<double>(kSampleRateHz) /
            static_cast<double>(cardlink::usb::kStreamUacRateHz));

        const unsigned output_words =
            nframes * cardlink::usb::kStreamUacChannels;
        unsigned written = 0u;
        while (written < output_words) {
          if (state.packet_words_used ==
              cardlink::usb::kStreamUacPacketWords) {
            auto &packet = state.packet;
            std::fill(packet.begin(), packet.end(), 0);
            packet[0] = static_cast<int16_t>(cardlink::usb::kStreamTagIdle);
            const uint8_t voice = mixer.HungriestUacWant(
                cardlink::usb::kStreamUacBodySamples);
            if (voice < kSampleVoices) {
              bool sof = false;
              uint8_t session = 0u;
              const uint16_t wave_id = mixer.LiveWave(voice);
              const unsigned got = mixer.FillUacFrame(
                  voice, packet.data() + 2u,
                  cardlink::usb::kStreamUacBodySamples, sof, session);
              if (got == cardlink::usb::kStreamUacBodySamples) {
                packet[0] = EncodeTag(voice, session, sof);
                packet[1] = static_cast<int16_t>(
                    mixer.RecordUacSubmission(voice,
                                              static_cast<uint16_t>(got)));
                mixer.CommitBurst(voice, session, wave_id, got, sof);
                if (sof) {
                  uint8_t order[kSampleVoices]{};
                  const unsigned voices = mixer.UrgentVoices(order);
                  double demand = 0.0;
                  double attack = 1.0e9;
                  for (unsigned i = 0u; i < voices; ++i) {
                    demand += mixer.VoiceSourceSamplesPerMs(order[i]);
                    attack = std::min(
                        attack, mixer.VoiceAttackLeadMs(order[i]));
                  }
                  state.urgent_voices.store(voices,
                                             std::memory_order_relaxed);
                  state.urgent_demand.store(demand,
                                             std::memory_order_relaxed);
                  state.urgent_attack_ms.store(
                      attack == 1.0e9 ? 0.0 : attack,
                      std::memory_order_relaxed);
                  state.urgent_quantum_ms.store(1.0,
                                                 std::memory_order_relaxed);
                }
              } else if (got != 0u) {
                mixer.AbortBurst(voice, session, wave_id, got, sof);
              }
            }
            state.packet_words_used = 0u;
          }
          const unsigned available = cardlink::usb::kStreamUacPacketWords -
                                     state.packet_words_used;
          const unsigned copy_n =
              std::min(available, output_words - written);
          std::copy_n(state.packet.data() + state.packet_words_used, copy_n,
                      dst + written);
          state.packet_words_used += copy_n;
          written += copy_n;
        }
        return 0;
      },
      this);
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
  impl_->running.store(true, std::memory_order_release);
  return true;
}

void SampleBulkOut::Stop()
{
  if (!impl_) {
    return;
  }
  impl_->running.store(false, std::memory_order_release);
  if (impl_->dac.isStreamRunning()) {
    impl_->dac.stopStream();
  }
  if (impl_->dac.isStreamOpen()) {
    impl_->dac.closeStream();
  }
  mixer_->DrainPendingCommands();
}

bool SampleBulkOut::Running() const
{
  return impl_ && impl_->running.load(std::memory_order_acquire);
}

uint32_t SampleBulkOut::XrunCount() const
{
  return impl_ ? impl_->xruns.load(std::memory_order_relaxed) : 0u;
}

uint64_t SampleBulkOut::RenderFrameCount() const
{
  return impl_ ? impl_->render_frames.load(std::memory_order_relaxed) : 0u;
}

UrgentScheduleStats SampleBulkOut::LastUrgentSchedule() const
{
  UrgentScheduleStats stats;
  if (!impl_) {
    return stats;
  }
  stats.voices = impl_->urgent_voices.load(std::memory_order_relaxed);
  stats.demand_samples_per_ms =
      impl_->urgent_demand.load(std::memory_order_relaxed);
  stats.remaining_attack_ms =
      impl_->urgent_attack_ms.load(std::memory_order_relaxed);
  stats.quantum_ms =
      impl_->urgent_quantum_ms.load(std::memory_order_relaxed);
  return stats;
}

void SampleBulkOut::SubmitStatus(const cardproto::VoiceQuery &status)
{
  if (!Running()) {
    return;
  }
  mixer_->ApplyVoiceStatus(status);
}

} // namespace cardlink::audio
