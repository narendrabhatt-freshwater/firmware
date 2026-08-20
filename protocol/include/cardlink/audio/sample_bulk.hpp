#pragma once

#include "cardlink/audio/sample_dry.hpp"

#include <memory>
#include <string>

namespace cardlink::audio {

/** Host thread: BODY bursts over vendor bulk. No ACK, no OS audio stack. */
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SampleDryMixer owned_;
  SampleDryMixer *mixer_ = &owned_;
};

} // namespace cardlink::audio
