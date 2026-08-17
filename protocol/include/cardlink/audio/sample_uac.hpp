#pragma once

#include "cardlink/audio/sample_dry.hpp"

#include <memory>
#include <string>

namespace cardlink::audio
{

/** RtAudio output to Channel Card UAC (10ch int16 @ 48 kHz). */
class SampleUacOut {
public:
  SampleUacOut();
  ~SampleUacOut();

  SampleUacOut(const SampleUacOut &) = delete;
  SampleUacOut &operator=(const SampleUacOut &) = delete;

  /** Open device whose name contains needle (default "Channel Card"). */
  bool Start(const std::string &name_needle, std::string &err);
  void Stop();
  bool Running() const;

  /** Render this mixer (default: owned). Bind before Start. */
  void BindMixer(SampleDryMixer &mixer);
  SampleDryMixer &Mixer() { return *mixer_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SampleDryMixer owned_;
  SampleDryMixer *mixer_ = &owned_;
};

} // namespace cardlink::audio
