#pragma once

#include "cmi/core.hpp"

#include "cardlink/audio/sample_bulk.hpp"
#include "cardlink/midi/midi_input.hpp"
#include "cardlink/midi/voice_bank.hpp"
#include "cardlink/rs485/bus.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/vm/compiler.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace cmi {

namespace detail {

Result Ok(std::string message = {});
Result Fail(ErrorCode code, std::string message);
Result FromCard(const cardproto::Result &card);
bool ValidVoice(uint8_t voice);
bool ValidKey(uint8_t key);

} // namespace detail

struct Core::Impl {
  explicit Impl(CoreParams initial);
  ~Impl();

  void Emit(const Result &result);
  Result RequireConnected() const;
  Result RequireVoiceReady(uint8_t voice) const;
  Result ResolveMidiPort();
  Result StartMidi();
  Result SilenceAndWaitForIdle();
  Result UploadProgram(uint8_t voice,
                       const cardlink::vm::CompileResult &compiled);

  Result RefreshAttackSamples();
  Result LoadSample(const SampleDefinition &sample);

  Result SampleNoteOn(uint8_t voice, uint8_t key, uint16_t sample_id);
  Result NoteOff(uint8_t voice);
  Result AllNotesOff();
  Result ApplyMidi(const cardlink::midi::NoteEvent &event);
  void PollVoiceStatus();

  void WorkerMain();
  void StartWorker();
  void Stop();

  CoreParams params;
  cardlink::rs485::Bus bus;
  cardlink::sample::Client samples;
  cardlink::audio::SampleBulkOut body;
  std::unique_ptr<cardlink::midi::MidiInput> midi;
  cardlink::midi::VoiceBank voices;
  std::array<uint16_t, 128> midi_sample{};
  std::array<bool, 256> attack_samples{};
  std::array<bool, 256> loaded_samples{};
  std::optional<unsigned> midi_index;
  Result last_sample_result;

  // Lock order is lifecycle, operation, then bus. The worker takes operation
  // then bus and never enters lifecycle code.
  std::mutex lifecycle_mutex;
  // Public operations take this lock before accessing mutable playback state.
  mutable std::mutex operation_mutex;
  // Card transactions are serialized independently from local state changes.
  std::mutex bus_mutex;
  std::mutex handler_mutex;
  ErrorHandler error_handler;
  std::atomic<bool> connected{false};
  std::atomic<uint8_t> loaded_programs{0u};
  std::atomic<bool> midi_open{false};
  std::atomic<bool> running{false};
  std::thread worker;
  uint8_t observed_active = 0;
};

} // namespace cmi
