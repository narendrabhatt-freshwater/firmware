#include "detail.hpp"

#include <algorithm>

namespace cmi {

using detail::Fail;
using detail::FromCard;
using detail::Ok;
using detail::ValidKey;
using detail::ValidVoice;

Result Core::Impl::SampleNoteOn(uint8_t voice, uint8_t key, uint16_t sample_id,
                                uint8_t velocity)
{
  const Result ready = RequireVoiceReady(voice);
  if (!ready) {
    return ready;
  }
  if (!ValidKey(key) || sample_id >= cardlink::audio::kSampleWaves || velocity == 0u ||
      velocity > 127u) {
    return Fail(ErrorCode::InvalidArgument,
                "invalid MIDI key, velocity, or sample id");
  }
  if (!loaded_samples[sample_id]) {
    return Fail(ErrorCode::SampleError,
                "sample " + std::to_string(sample_id) + " is not loaded");
  }
  if (!body.Running()) {
    std::string error;
    if (!body.Start(params.channel_audio_device, error)) {
      return Fail(ErrorCode::AudioError, error);
    }
  }

  last_sample_result = Ok();
  const cardlink::sample::NoteRequest note{
      voice, key, velocity, sample_id, 0xFFu, true};
  if (!samples.NoteOnBatch(&note, 1u)) {
    return last_sample_result.ok()
               ? Fail(ErrorCode::SampleError,
                      "sample could not be prepared for playback")
               : last_sample_result;
  }
  return last_sample_result;
}

Result Core::Impl::NoteOff(uint8_t voice)
{
  const Result connection = RequireConnected();
  if (!connection) {
    return connection;
  }
  if (!ValidVoice(voice)) {
    return Fail(ErrorCode::InvalidArgument, "voice must be 0..7");
  }
  cardproto::Result card;
  {
    std::lock_guard<std::mutex> lock(bus_mutex);
    card = bus.Channel().NoteOff(voice);
  }
  if (card.ok()) {
    samples.Mixer().NoteOff(voice);
  }
  return FromCard(card);
}

Result Core::Impl::AllNotesOff()
{
  const Result connection = RequireConnected();
  if (!connection) {
    return connection;
  }
  cardproto::Result card;
  {
    std::lock_guard<std::mutex> lock(bus_mutex);
    card = bus.Channel().AllNotesOff();
  }
  if (card.ok()) {
    (void)voices.AllOff();
    for (uint8_t voice = 0; voice < 8u; ++voice) {
      samples.Mixer().NoteOff(voice);
    }
  }
  return FromCard(card);
}

Result Core::Impl::ApplyMidi(const cardlink::midi::NoteEvent &event)
{
  if (loaded_programs.load() != 0xFFu) {
    return Fail(ErrorCode::NotReady,
                "MIDI note ignored until all voice programs are loaded");
  }
  if (event.action == cardlink::midi::NoteAction::AllOff) {
    return AllNotesOff();
  }

  const auto events = event.action == cardlink::midi::NoteAction::On
                          ? voices.NoteOn(event.key, event.velocity)
                          : voices.NoteOff(event.key);
  for (const auto &bank_event : events) {
    Result result = Ok();
    switch (bank_event.kind) {
    case cardlink::midi::BankEventKind::Off:
      result = NoteOff(bank_event.slot);
      break;
    case cardlink::midi::BankEventKind::On:
    case cardlink::midi::BankEventKind::Retrig:
      result = SampleNoteOn(bank_event.slot, bank_event.midi_key,
                            midi_sample[bank_event.midi_key],
                            bank_event.velocity);
      break;
    case cardlink::midi::BankEventKind::Steal:
      continue;
    }
    if (!result) {
      return result;
    }
  }
  return Ok();
}

void Core::Impl::PollVoiceStatus()
{
  cardproto::Result reply;
  {
    std::lock_guard<std::mutex> lock(bus_mutex);
    reply = bus.Channel().QueryVoiceStatus();
  }
  if (!reply.ok()) {
    Emit(FromCard(reply));
    return;
  }
  cardproto::VoiceQuery status;
  if (!cardproto::ParseVoiceQuery(reply.raw, status)) {
    Emit(Fail(ErrorCode::BadReply, "invalid Channel voice status"));
    return;
  }
  if (body.Running()) {
    body.SubmitStatus(status);
  }
  const uint8_t now = status.active_mask | status.pending_mask;
  const uint8_t became_idle = observed_active & static_cast<uint8_t>(~now);
  for (uint8_t voice = 0; voice < 8u; ++voice) {
    if ((became_idle & static_cast<uint8_t>(1u << voice)) != 0u) {
      samples.Silence(voice);
    }
  }
  observed_active = now;
  if (now == 0u && body.Running()) {
    body.Stop();
  }
}

Result Core::sampleNoteOn(uint8_t voice, uint8_t midi_key, uint16_t sample_id,
                          uint8_t velocity)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  return impl_->SampleNoteOn(voice, midi_key, sample_id, velocity);
}

Result Core::noteOff(uint8_t voice)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  return impl_->NoteOff(voice);
}

Result Core::allNotesOff()
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  return impl_->AllNotesOff();
}

Result Core::setMidiSampleMap(const std::array<uint16_t, 128> &sample_ids)
{
  for (const uint16_t id : sample_ids) {
    if (id >= cardlink::audio::kSampleWaves) {
      return Fail(ErrorCode::InvalidArgument,
                  "MIDI sample ids must be 0..247");
    }
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  impl_->midi_sample = sample_ids;
  return Ok("MIDI sample map updated");
}

} // namespace cmi
