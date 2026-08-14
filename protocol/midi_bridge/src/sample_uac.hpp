#pragma once

#include "cardlink/audio/sample_dry.hpp"

#include <memory>
#include <string>

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
  void BindMixer(cardlink::audio::SampleDryMixer &mixer);
  cardlink::audio::SampleDryMixer &Mixer() { return *mixer_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  cardlink::audio::SampleDryMixer owned_;
  cardlink::audio::SampleDryMixer *mixer_ = &owned_;
};
