#include "audio_engine.hpp"

#include <RtAudio.h>

#include <cmath>
#include <cstring>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace midi_host
{

struct AudioEngine::Impl
{
  RtAudio dac;
};

AudioEngine::AudioEngine()
    : impl_(new Impl)
{
}

AudioEngine::~AudioEngine()
{
  Stop();
  delete impl_;
  impl_ = nullptr;
}

void AudioEngine::Start(unsigned sample_rate)
{
  if (running_) {
    return;
  }

  if (impl_->dac.getDeviceCount() < 1) {
    throw std::runtime_error("no audio output devices found");
  }

  const unsigned default_out = impl_->dac.getDefaultOutputDevice();
  RtAudio::DeviceInfo info = impl_->dac.getDeviceInfo(default_out);
  device_name_ = info.name;
  sample_rate_ = sample_rate;

  RtAudio::StreamParameters params;
  params.deviceId = default_out;
  params.nChannels = 2;
  params.firstChannel = 0;

  unsigned buffer_frames = 256;
  RtAudioErrorType err = impl_->dac.openStream(
      &params,
      nullptr,
      RTAUDIO_FLOAT32,
      sample_rate_,
      &buffer_frames,
      &AudioEngine::AudioCallback,
      this);

  if (err != RTAUDIO_NO_ERROR) {
    // Fall back to device preferred rate if 48 kHz open failed.
    if (!info.preferredSampleRate) {
      throw std::runtime_error(std::string("RtAudio openStream failed: ") +
                               impl_->dac.getErrorText());
    }
    sample_rate_ = static_cast<unsigned>(info.preferredSampleRate);
    buffer_frames = 256;
    err = impl_->dac.openStream(&params,
                                nullptr,
                                RTAUDIO_FLOAT32,
                                sample_rate_,
                                &buffer_frames,
                                &AudioEngine::AudioCallback,
                                this);
    if (err != RTAUDIO_NO_ERROR) {
      throw std::runtime_error(std::string("RtAudio openStream failed: ") +
                               impl_->dac.getErrorText());
    }
  }

  err = impl_->dac.startStream();
  if (err != RTAUDIO_NO_ERROR) {
    impl_->dac.closeStream();
    throw std::runtime_error(std::string("RtAudio startStream failed: ") +
                             impl_->dac.getErrorText());
  }

  running_ = true;
}

void AudioEngine::Stop()
{
  if (!impl_) {
    return;
  }
  if (impl_->dac.isStreamRunning()) {
    impl_->dac.stopStream();
  }
  if (impl_->dac.isStreamOpen()) {
    impl_->dac.closeStream();
  }
  running_ = false;
}

void AudioEngine::ApplyBankEvent(const BankEvent& event)
{
  std::lock_guard<std::mutex> lock(voices_mu_);
  VoiceAudioState& v = voices_[event.slot];

  switch (event.kind) {
  case BankEventKind::On:
    v.active = true;
    v.freq_hz = event.freq_hz;
    v.retrigger = true;
    break;
  case BankEventKind::Retrig:
    v.active = true;
    v.freq_hz = event.freq_hz;
    v.retrigger = true;
    break;
  case BankEventKind::Steal:
    // Slot is reused by the following On in the same NoteOn(); ignore here
    // so we do not briefly silence the voice between the two events.
    break;
  case BankEventKind::Off:
    v.active = false;
    v.freq_hz = 0.0;
    v.retrigger = false;
    break;
  }
}

int AudioEngine::AudioCallback(void* output,
                               void* /*input*/,
                               unsigned int n_frames,
                               double /*stream_time*/,
                               unsigned int /*status*/,
                               void* user_data)
{
  auto* self = static_cast<AudioEngine*>(user_data);
  self->Render(static_cast<float*>(output), n_frames);
  return 0;
}

void AudioEngine::Render(float* out, unsigned n_frames)
{
  std::array<VoiceAudioState, kVoiceCount> local{};
  {
    std::lock_guard<std::mutex> lock(voices_mu_);
    local = voices_;
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (voices_[i].retrigger) {
        voices_[i].retrigger = false;
      }
    }
  }

  for (uint8_t i = 0; i < kVoiceCount; ++i) {
    if (local[i].retrigger) {
      phase_[i] = 0.0;
    }
  }

  constexpr double kVoiceGain = 0.125; /* match channel RS485 1/8 scale */
  const double two_pi = 2.0 * M_PI;

  for (unsigned frame = 0; frame < n_frames; ++frame) {
    double mix = 0.0;
    for (uint8_t i = 0; i < kVoiceCount; ++i) {
      if (!local[i].active || local[i].freq_hz <= 0.0) {
        continue;
      }
      mix += std::sin(phase_[i]) * kVoiceGain;
      phase_[i] += two_pi * local[i].freq_hz / static_cast<double>(sample_rate_);
      if (phase_[i] >= two_pi) {
        phase_[i] -= two_pi;
      }
    }
    const float s = static_cast<float>(mix);
    out[frame * 2 + 0] = s;
    out[frame * 2 + 1] = s;
  }
}

} // namespace midi_host
