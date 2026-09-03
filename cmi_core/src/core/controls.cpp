#include "detail.hpp"

#include <cstdio>

namespace cmi {
namespace {

bool ValidCutoff(double hz)
{
  return hz == 0.0 || (hz >= 20.0 && hz <= 20000.0);
}

bool ValidQ(double q) { return q >= 0.5 && q <= 10.0; }

bool ValidTracking(double amount)
{
  return amount >= 0.0 && amount <= 10.0;
}

} // namespace

using detail::Fail;
using detail::FromCard;
using detail::Ok;
using detail::ValidVoice;

Result Core::setAttenuation(uint8_t attenuation_db)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  if (attenuation_db > 127u) {
    return Fail(ErrorCode::InvalidArgument,
                "attenuation must be 0..127 dB");
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Channel().SetGain(1u, attenuation_db));
}

Result Core::setFilter(uint8_t voice, double cutoff_hz, double q)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  if (!ValidVoice(voice) || !ValidCutoff(cutoff_hz) || !ValidQ(q)) {
    return Fail(ErrorCode::InvalidArgument, "invalid filter parameters");
  }
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Channel().SetFilter(voice, cutoff_hz, q));
}

Result Core::setAllFilters(double cutoff_hz, double q)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  if (!ValidCutoff(cutoff_hz) || !ValidQ(q)) {
    return Fail(ErrorCode::InvalidArgument, "invalid filter parameters");
  }
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Channel().SetFilters(cutoff_hz, q));
}

Result Core::setFilterPitchTracking(uint8_t voice, double amount)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  if (!ValidVoice(voice) || !ValidTracking(amount)) {
    return Fail(ErrorCode::InvalidArgument,
                "invalid filter pitch-tracking parameters");
  }
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Channel().SetFk(voice, amount));
}

Result Core::setAllFilterPitchTracking(double amount)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  if (!ValidTracking(amount)) {
    return Fail(ErrorCode::InvalidArgument,
                "filter pitch tracking must be 0..10");
  }
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Channel().SetFk(amount));
}

Result Core::queryVoiceStatus(VoiceStatus &status)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  cardproto::Result reply;
  {
    std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
    reply = impl_->bus.Channel().QueryVoiceStatus();
  }
  if (!reply.ok()) {
    return FromCard(reply);
  }
  cardproto::VoiceQuery parsed;
  if (!cardproto::ParseVoiceQuery(reply.raw, parsed)) {
    return Fail(ErrorCode::BadReply, "invalid Channel voice status");
  }
  status.active_mask = parsed.active_mask;
  status.pending_mask = parsed.pending_mask;
  status.refill_voice = parsed.best;
  status.capacity = parsed.capacity;
  status.session = parsed.target_session;
  status.fill = parsed.target_fill;
  status.free_samples = parsed.free_samples;
  return Ok("Channel voice status updated");
}

Result Core::recover()
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return impl_->bus.SoftRecover()
             ? Ok("RS485 bus recovered")
             : Fail(ErrorCode::IoError, "RS485 recovery failed");
}

Result Core::queryEffectStatus(EffectStatus &status)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  cardproto::Result card;
  {
    std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
    card = impl_->bus.Effect().Status();
  }
  if (!card.ok()) {
    return FromCard(card);
  }
  unsigned phantom = 0;
  unsigned power_good = 0;
  unsigned audio = 0;
  unsigned led = 0;
  unsigned echo = 0;
  if (std::sscanf(card.raw, "ok: v %u pg %u a %u l %u ec %u", &phantom,
                  &power_good, &audio, &led, &echo) != 5) {
    return Fail(ErrorCode::BadReply, "invalid Effect Card status");
  }
  status.phantom_enabled = phantom != 0u;
  status.phantom_power_good = power_good != 0u;
  status.audio_enabled = audio != 0u;
  return Ok("Effect Card status updated");
}

Result Core::setPhantomPower(bool enabled)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Effect().Set48V(enabled));
}

Result Core::setEffectAudioEnabled(bool enabled)
{
  std::lock_guard<std::mutex> operation_lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  std::lock_guard<std::mutex> bus_lock(impl_->bus_mutex);
  return FromCard(impl_->bus.Effect().SetAudioEn(enabled));
}

} // namespace cmi
