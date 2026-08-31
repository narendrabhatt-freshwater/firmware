#pragma once

#include "cardlink/midi/voice_bank.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace cardlink::audio
{

struct VoiceAudioState
{
  bool active = false;
  bool retrigger = false;
  double freq_hz = 0.0;
};

/**
 * RtAudio output: mixes up to kVoiceCount sines to the default speaker.
 * Voice state is updated from the main thread under a mutex; the audio
 * callback copies once per buffer and renders without blocking long.
 */
class AudioEngine
{
public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /** Open default output and start streaming. Throws std::runtime_error on failure. */
  void Start(unsigned sample_rate = 48000);

  void Stop();

  /** Apply a bank event to the audio voice table (on/off/steal/retrig). */
  void ApplyBankEvent(const midi::BankEvent& event, double resolved_hz = 0.0);

  unsigned SampleRate() const { return sample_rate_; }
  std::string DeviceName() const { return device_name_; }

private:
  static int AudioCallback(void* output,
                           void* /*input*/,
                           unsigned int n_frames,
                           double /*stream_time*/,
                           unsigned int status,
                           void* user_data);

  void Render(float* out, unsigned n_frames);

  struct Impl;
  Impl* impl_ = nullptr;

  std::mutex voices_mu_;
  std::array<VoiceAudioState, midi::kVoiceCount> voices_{};

  std::array<double, midi::kVoiceCount> phase_{};
  unsigned sample_rate_ = 48000;
  std::string device_name_;
  std::atomic<bool> running_{false};
};

} // namespace cardlink::audio
