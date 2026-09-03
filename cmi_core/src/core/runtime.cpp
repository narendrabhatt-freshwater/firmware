#include "detail.hpp"

#include "cardlink/serial_port.hpp"
#include "cardlink/usb/cdc_port.hpp"
#include "cardlink/vm/uploader.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace cmi {

using detail::Fail;
using detail::FromCard;
using detail::Ok;

Core::Impl::Impl(CoreParams initial) : params(std::move(initial))
{
  for (uint16_t key = 0; key < midi_sample.size(); ++key) {
    midi_sample[key] = key;
  }
  samples.SetCdcPath(params.channel_cdc_port);
  body.BindMixer(samples.Mixer());

  samples.SetConsole([this](const std::string &command) {
    std::lock_guard<std::mutex> lock(bus_mutex);
    last_sample_result = FromCard(bus.Channel().Exec(command));
  });
  samples.SetNoteGate(
      [this](const cardlink::sample::NoteRequest &note,
             cardlink::sample::Client::NoteGateStart start,
             cardlink::sample::Client::NoteGateDone done) {
        start();
        cardproto::Result card;
        {
          std::lock_guard<std::mutex> lock(bus_mutex);
          if (note.note_on) {
            card = bus.Channel().Exec(
                "aw " + std::to_string(note.voice) + " " +
                std::to_string(note.wave_id));
            if (card.ok()) {
              card = bus.Channel().StreamNoteOn(
                  note.voice, note.key, note.velocity, note.session);
            }
          } else {
            card = bus.Channel().NoteOff(note.voice);
          }
        }
        last_sample_result = FromCard(card);
        done(card.ok());
        return card.ok();
      });
}

Core::Impl::~Impl() { Stop(); }

void Core::Impl::Emit(const Result &result)
{
  if (result.ok()) {
    return;
  }
  ErrorHandler handler;
  {
    std::lock_guard<std::mutex> lock(handler_mutex);
    handler = error_handler;
  }
  if (handler) {
    handler(result);
  }
}

Result Core::Impl::RequireConnected() const
{
  return connected.load()
             ? Ok()
             : Fail(ErrorCode::NotConnected, "cmi::Core is not connected");
}

Result Core::Impl::RequireVoiceReady(uint8_t voice) const
{
  const Result connection = RequireConnected();
  if (!connection) {
    return connection;
  }
  if (!detail::ValidVoice(voice)) {
    return Fail(ErrorCode::InvalidArgument, "voice must be 0..7");
  }
  return (loaded_programs.load() & static_cast<uint8_t>(1u << voice)) != 0u
             ? Ok()
             : Fail(ErrorCode::NotReady,
                    "load a Channel VM program for voice " +
                        std::to_string(static_cast<unsigned>(voice)));
}

Result Core::Impl::ResolveMidiPort()
{
  midi_index.reset();
  midi.reset();
  if (params.midi_port.empty()) {
    return Ok();
  }
  std::vector<cardlink::midi::MidiPortInfo> ports;
  try {
    midi = std::make_unique<cardlink::midi::MidiInput>();
    ports = cardlink::midi::MidiInput::ListPorts();
  } catch (const std::exception &error) {
    midi.reset();
    return Fail(ErrorCode::MidiError, error.what());
  }
  for (const auto &port : ports) {
    if (port.name == params.midi_port) {
      midi_index = port.index;
      return Ok();
    }
  }
  return Fail(ErrorCode::MidiError,
              "MIDI input not found: " + params.midi_port);
}

Result Core::Impl::StartMidi()
{
  if (!midi_index || midi_open.load() || loaded_programs.load() != 0xFFu) {
    return Ok();
  }
  try {
    midi->Open(*midi_index);
    midi_open.store(true);
    return Ok("MIDI input opened: " + midi->PortName());
  } catch (const std::exception &error) {
    midi_open.store(false);
    return Fail(ErrorCode::MidiError, error.what());
  }
}

Result Core::Impl::SilenceAndWaitForIdle()
{
  const Result silence = AllNotesOff();
  if (!silence) {
    return silence;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    cardproto::Result reply;
    {
      std::lock_guard<std::mutex> lock(bus_mutex);
      reply = bus.Channel().QueryVoiceStatus();
    }
    if (!reply.ok()) {
      return FromCard(reply);
    }
    cardproto::VoiceQuery status;
    if (!cardproto::ParseVoiceQuery(reply.raw, status)) {
      return Fail(ErrorCode::BadReply, "invalid Channel voice status");
    }
    if (status.active_mask == 0u && status.pending_mask == 0u) {
      return Ok();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return Fail(ErrorCode::Timeout,
              "Channel voices did not become idle before loading");
}

Result Core::Impl::UploadProgram(
    uint8_t voice, const cardlink::vm::CompileResult &compiled)
{
  if (!compiled.ok) {
    return Fail(ErrorCode::VmError, compiled.message);
  }

  const Result silence = SilenceAndWaitForIdle();
  if (!silence) {
    return silence;
  }

  if (midi) {
    midi->Close();
  }
  midi_open.store(false);

  cardlink::SerialPort port;
  std::string error;
  if (!cardlink::usb::OpenCdcPort(port, params.channel_cdc_port, error)) {
    (void)StartMidi();
    return Fail(ErrorCode::IoError, error);
  }
  cardlink::vm::VmUploader uploader(port);
  const auto uploaded = uploader.Upload(
      voice, compiled.program.data(), compiled.program.size());
  port.Close();
  if (!uploaded.ok) {
    (void)StartMidi();
    return Fail(ErrorCode::VmError, uploaded.message);
  }
  loaded_programs.fetch_or(static_cast<uint8_t>(1u << voice));
  const Result midi_result = StartMidi();
  return midi_result
             ? Ok("VM program loaded for voice " +
                  std::to_string(static_cast<unsigned>(voice)))
             : midi_result;
}

void Core::Impl::WorkerMain()
{
  auto next_status = std::chrono::steady_clock::now();
  while (running.load()) {
    std::vector<cardlink::midi::NoteEvent> events;
    if (midi_open.load()) {
      midi->Poll(events);
    }
    for (const auto &event : events) {
      Result result;
      {
        std::lock_guard<std::mutex> lock(operation_mutex);
        result = ApplyMidi(event);
      }
      Emit(result);
    }

    const auto now = std::chrono::steady_clock::now();
    if (body.Running() && now >= next_status) {
      std::lock_guard<std::mutex> lock(operation_mutex);
      if (connected.load()) {
        PollVoiceStatus();
      }
      next_status = now + std::chrono::milliseconds(5);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void Core::Impl::StartWorker()
{
  if (running.exchange(true)) {
    return;
  }
  worker = std::thread([this] { WorkerMain(); });
}

void Core::Impl::Stop()
{
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
  running.store(false);
  if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
    worker.join();
  }
  std::lock_guard<std::mutex> operation_lock(operation_mutex);
  if (midi) {
    midi->Close();
  }
  midi_open.store(false);
  body.Stop();
  if (connected.load()) {
    std::lock_guard<std::mutex> bus_lock(bus_mutex);
    bus.Close();
  }
  connected.store(false);
  loaded_programs.store(0u);
  observed_active = 0u;
}

} // namespace cmi
