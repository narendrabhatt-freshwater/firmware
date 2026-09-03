#pragma once

#include "cardlink/audio/sample_dry.hpp"
#include "cardproto/channel.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace cardlink::audio {

struct UrgentScheduleStats {
  unsigned voices = 0u;
  double demand_samples_per_ms = 0.0;
  double remaining_attack_ms = 0.0;
  double quantum_ms = 0.0;
};

/** Packed BODY over the class-compliant 10ch int16 UAC2 output. */
class SampleBulkOut {
public:
  SampleBulkOut();
  ~SampleBulkOut();

  SampleBulkOut(const SampleBulkOut &) = delete;
  SampleBulkOut &operator=(const SampleBulkOut &) = delete;

  void BindMixer(SampleDryMixer &mixer);
  SampleDryMixer &Mixer() { return *mixer_; }

  bool Start(std::string &err);
  /** Start on a matching device, or auto-detect when device_name is empty. */
  bool Start(const std::string &device_name, std::string &err);
  void Stop();
  bool Running() const;
  /** CoreAudio/RtAudio callback discontinuities in the current stream. */
  uint32_t XrunCount() const;
  /** Audio frames requested by CoreAudio in the current stream generation. */
  uint64_t RenderFrameCount() const;
  /** Most recent first-SOF urgent scheduling decision. */
  UrgentScheduleStats LastUrgentSchedule() const;

  /** Feed one authoritative RS-485 vq snapshot to the refill scheduler. */
  void SubmitStatus(const cardproto::VoiceQuery &status);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SampleDryMixer owned_;
  SampleDryMixer *mixer_ = &owned_;
};

} // namespace cardlink::audio
