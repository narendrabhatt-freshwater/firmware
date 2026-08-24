#pragma once

#include "cardlink/audio/sample_dry.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace cardlink::audio {

/** Host thread: packed BODY over vendor bulk. No ACK, no OS audio stack. */
class SampleBulkOut {
public:
  SampleBulkOut();
  ~SampleBulkOut();

  SampleBulkOut(const SampleBulkOut &) = delete;
  SampleBulkOut &operator=(const SampleBulkOut &) = delete;

  void BindMixer(SampleDryMixer &mixer);
  SampleDryMixer &Mixer() { return *mixer_; }

  bool Start(std::string &err);
  void Stop();
  bool Running() const;

  /** Feed one authoritative RS-485 vq snapshot to the refill scheduler. */
  void SubmitStatus(uint8_t mask, uint8_t best,
                    const std::array<uint16_t, kSampleVoices> &free_samples,
                    uint16_t last_pack_sequence);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SampleDryMixer owned_;
  SampleDryMixer *mixer_ = &owned_;
};

} // namespace cardlink::audio
