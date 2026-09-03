#include "detail.hpp"

#include <algorithm>

namespace cmi {

using detail::Fail;
using detail::FromCard;
using detail::Ok;
using detail::ValidKey;
using detail::ValidVoice;

Result Core::Impl::NoteOn(uint8_t voice, uint8_t key)
{
  const Result ready = RequireVoiceReady(voice);
  if (!ready) {
    return ready;
  }
  if (!ValidKey(key)) {
    return Fail(ErrorCode::InvalidArgument, "MIDI key must be 0..127");
  }
  const auto empty_sample =
      std::find(attack_samples.begin(), attack_samples.end(), false);
  if (empty_sample == attack_samples.end()) {
    return Fail(ErrorCode::SampleError,
                "oscillator playback requires one unused sample slot");
  }
  const size_t sample_id = static_cast<size_t>(
      std::distance(attack_samples.begin(), empty_sample));
  std::lock_guard<std::mutex> lock(bus_mutex);
  cardproto::Result card = bus.Channel().Exec(
      "aw " + std::to_string(voice) + " " + std::to_string(sample_id));
  if (card.ok()) {
    card = bus.Channel().NoteOn(voice, key);
  }
  return FromCard(card);
}

Result Core::Impl::SampleNoteOn(uint8_t voice, uint8_t key, uint16_t sample_id)
{
  const Result ready = RequireVoiceReady(voice);
  if (!ready) {
    return ready;
  }
  if (!ValidKey(key) || sample_id >= 256u) {
    return Fail(ErrorCode::InvalidArgument, "invalid MIDI key or sample id");
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
      voice, key, sample_id, 0xFFu, true};
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
                          ? voices.NoteOn(event.key)
                          : voices.NoteOff(event.key);
  for (const auto &bank_event : events) {
    Result result = Ok();
    switch (bank_event.kind) {
    case cardlink::midi::BankEventKind::Off:
      result = NoteOff(bank_event.slot);
      break;
    case cardlink::midi::BankEventKind::On:
    case cardlink::midi::BankEventKind::Retrig:
      result = midi_playback == MidiPlayback::Oscillator
                   ? NoteOn(bank_event.slot, bank_event.midi_key)
                   : SampleNoteOn(bank_event.slot, bank_event.midi_key,
                                  midi_sample[bank_event.midi_key]);
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

Result Core::noteOn(uint8_t voice, uint8_t midi_key)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  return impl_->NoteOn(voice, midi_key);
}

Result Core::sampleNoteOn(uint8_t voice, uint8_t midi_key, uint16_t sample_id)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  return impl_->SampleNoteOn(voice, midi_key, sample_id);
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

Result Core::setMidiPlayback(MidiPlayback playback)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  const Result silence = impl_->AllNotesOff();
  if (!silence) {
    return silence;
  }
  impl_->midi_playback = playback;
  return Ok(playback == MidiPlayback::Oscillator
                ? "MIDI oscillator playback enabled"
                : "MIDI sample playback enabled");
}

Result Core::setMidiSampleMap(const std::array<uint16_t, 128> &sample_ids)
{
  for (const uint16_t id : sample_ids) {
    if (id >= 256u) {
      return Fail(ErrorCode::InvalidArgument,
                  "MIDI sample ids must be 0..255");
    }
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  impl_->midi_sample = sample_ids;
  return Ok("MIDI sample map updated");
}

} // namespace cmi
