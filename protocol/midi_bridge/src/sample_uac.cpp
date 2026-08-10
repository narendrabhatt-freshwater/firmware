#include "sample_uac.hpp"

#include <RtAudio.h>

#include <cstring>
#include <string>
#include <vector>

struct SampleUacOut::Impl {
  RtAudio dac;
};

SampleUacOut::SampleUacOut() : impl_(std::make_unique<Impl>()) {}

SampleUacOut::~SampleUacOut() { Stop(); }

namespace {

int AudioCb(void *out,
            void * /*in*/,
            unsigned int nframes,
            double /*time*/,
            RtAudioStreamStatus /*status*/,
            void *user)
{
  auto *self = static_cast<SampleUacOut *>(user);
  auto *dst = static_cast<int16_t *>(out);
  if (!dst || !self) {
    return 0;
  }
  self->Mixer().Render(dst, nframes);
  return 0;
}

bool NameMatches(const std::string &name, const std::string &needle)
{
  if (!needle.empty() && name.find(needle) != std::string::npos) {
    return true;
  }
  /* CoreAudio uses the UAC interface string; older builds used this label. */
  if (name.find("SAMPLE dry") != std::string::npos) {
    return true;
  }
  /* RtAudio 6 often prefixes "Freshwater: …". */
  if (name.find("Channel Card") != std::string::npos) {
    return true;
  }
  return false;
}

} // namespace

bool SampleUacOut::Start(const std::string &name_needle, std::string &err)
{
  Stop();
  if (!impl_) {
    err = "no RtAudio";
    return false;
  }

  /* RtAudio 6: device IDs are opaque — never index 0..count-1. */
  const std::vector<unsigned> ids = impl_->dac.getDeviceIds();
  unsigned dev = 0;
  bool found = false;
  std::string seen;
  for (unsigned id : ids) {
    RtAudio::DeviceInfo info = impl_->dac.getDeviceInfo(id);
    if (info.outputChannels < cardlink::audio::kSampleVoices) {
      continue;
    }
    if (!seen.empty()) {
      seen += ", ";
    }
    seen += info.name;
    if (NameMatches(info.name, name_needle)) {
      dev = id;
      found = true;
      break;
    }
  }
  if (!found) {
    err = "no UAC device matching \"" + name_needle + "\"";
    if (!seen.empty()) {
      err += " (8ch outs: " + seen + ")";
    } else if (!ids.empty()) {
      err += " (no 8ch output among " + std::to_string(ids.size()) +
             " devices)";
    }
    return false;
  }

  RtAudio::StreamParameters params;
  params.deviceId = dev;
  params.nChannels = cardlink::audio::kSampleVoices;
  params.firstChannel = 0;

  unsigned buffer_frames = 128;
  RtAudioErrorType e = impl_->dac.openStream(
      &params, nullptr, RTAUDIO_SINT16, cardlink::audio::kSampleRateHz,
      &buffer_frames, &AudioCb, this);
  if (e != RTAUDIO_NO_ERROR) {
    err = impl_->dac.getErrorText();
    return false;
  }
  e = impl_->dac.startStream();
  if (e != RTAUDIO_NO_ERROR) {
    err = impl_->dac.getErrorText();
    impl_->dac.closeStream();
    return false;
  }
  return true;
}

void SampleUacOut::Stop()
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
}

bool SampleUacOut::Running() const
{
  return impl_ && impl_->dac.isStreamRunning();
}
