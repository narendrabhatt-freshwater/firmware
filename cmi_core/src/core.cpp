#include "core/detail.hpp"

#include <utility>

namespace cmi {
namespace detail {

Result Ok(std::string message)
{
  return {ErrorCode::Ok, std::move(message), {}};
}

Result Fail(ErrorCode code, std::string message)
{
  return {code, std::move(message), {}};
}

Result FromCard(const cardproto::Result &card)
{
  ErrorCode code = ErrorCode::CardError;
  switch (card.status) {
  case cardproto::Status::Ok:
    code = ErrorCode::Ok;
    break;
  case cardproto::Status::Timeout:
    code = ErrorCode::Timeout;
    break;
  case cardproto::Status::IoError:
    code = ErrorCode::IoError;
    break;
  case cardproto::Status::BadReply:
    code = ErrorCode::BadReply;
    break;
  case cardproto::Status::Err:
  case cardproto::Status::BusBusy:
    code = ErrorCode::CardError;
    break;
  }

  const std::string reply = card.raw;
  std::string message;
  if (code == ErrorCode::Ok) {
    message = reply.empty() ? "ok" : reply;
  } else if (card.err_code[0] != '\0') {
    message = card.err_code;
  } else {
    message = reply.empty() ? "card operation failed" : reply;
  }
  return {code, std::move(message), reply};
}

bool ValidVoice(uint8_t voice) { return voice < 8u; }
bool ValidKey(uint8_t key) { return key < 128u; }

} // namespace detail

using detail::Fail;
using detail::FromCard;
using detail::Ok;

Core::Core(CoreParams params) : impl_(std::make_unique<Impl>(std::move(params)))
{
}

Core::~Core() = default;

std::vector<MidiPort> Core::listMidiPorts()
{
  std::vector<MidiPort> result;
  try {
    for (const auto &port : cardlink::midi::MidiInput::ListPorts()) {
      result.push_back({port.index, port.name});
    }
  } catch (...) {
    return {};
  }
  return result;
}

Result Core::connect()
{
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  if (impl_->connected.load()) {
    return Ok("already connected");
  }
  if (impl_->params.rs485_port.empty() ||
      impl_->params.channel_cdc_port.empty()) {
    return Fail(ErrorCode::InvalidArgument,
                "rs485_port and channel_cdc_port are required");
  }
  if (impl_->params.attenuation_db > 127u) {
    return Fail(ErrorCode::InvalidArgument,
                "attenuation_db must be 0..127");
  }

  Result result = impl_->ResolveMidiPort();
  if (!result) {
    return result;
  }

  cardlink::rs485::BusOptions options;
  options.baud = impl_->params.baud;
  options.atten_db = impl_->params.attenuation_db;
  options.effect_echo = cardlink::rs485::EffectEcho::Off;
  options.allow_missing_effect = true;
  const cardproto::Result opened =
      impl_->bus.Open(impl_->params.rs485_port, options);
  if (!opened.ok()) {
    return FromCard(opened);
  }
  impl_->connected.store(true);

  result = impl_->RefreshAttackSamples();
  if (!result) {
    impl_->bus.Close();
    impl_->connected.store(false);
    return result;
  }

  impl_->StartWorker();
  return Ok("CMI core connected; load a VM program for each voice in use");
}

Result Core::disconnect()
{
  impl_->Stop();
  return Ok("CMI core disconnected");
}

bool Core::isConnected() const { return impl_->connected.load(); }

bool Core::isReady() const
{
  const bool midi_ready =
      impl_->params.midi_port.empty() || impl_->midi_open.load();
  return impl_->connected.load() && impl_->loaded_programs.load() == 0xFFu &&
         midi_ready;
}

uint8_t Core::loadedProgramMask() const
{
  return impl_->loaded_programs.load();
}

void Core::setErrorHandler(ErrorHandler handler)
{
  std::lock_guard<std::mutex> lock(impl_->handler_mutex);
  impl_->error_handler = std::move(handler);
}

Result Core::loadVoiceScript(uint8_t voice, const std::string &path)
{
  if (!detail::ValidVoice(voice)) {
    return Fail(ErrorCode::InvalidArgument, "voice must be 0..7");
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  if (path.empty()) {
    return Fail(ErrorCode::InvalidArgument, "VM program path is required");
  }
  cardlink::vm::BerryCompiler compiler;
  return impl_->UploadProgram(voice, compiler.CompileChannelFile(path));
}

Result Core::loadVoiceScriptSource(uint8_t voice, const std::string &source)
{
  if (!detail::ValidVoice(voice)) {
    return Fail(ErrorCode::InvalidArgument, "voice must be 0..7");
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  if (source.empty()) {
    return Fail(ErrorCode::InvalidArgument, "VM program source is required");
  }
  cardlink::vm::BerryCompiler compiler;
  return impl_->UploadProgram(voice, compiler.CompileChannel(source));
}

} // namespace cmi
