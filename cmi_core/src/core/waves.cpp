#include "detail.hpp"

#include "cardlink/audio/wave_loader.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace cmi {
namespace {

using detail::Fail;
using detail::Ok;

int HexNibble(char value)
{
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

Result ValidateSample(const SampleDefinition &sample)
{
  if (sample.id >= 256u || !(sample.root_hz > 0.0)) {
    return Fail(ErrorCode::InvalidArgument,
                "sample id must be 0..255 and root_hz must be positive");
  }
  const bool combined = !sample.sample_file.empty();
  const bool split = !sample.attack_file.empty() || !sample.body_file.empty();
  if (combined == split) {
    return Fail(ErrorCode::InvalidArgument,
                "provide either sample_file or an attack/body pair");
  }

  std::string error;
  if (combined) {
    cardlink::audio::LoadedWave loaded;
    if (!cardlink::audio::LoadWaveFile(sample.sample_file,
                                       sample.raw_sample_rate_hz, loaded,
                                       error)) {
      return Fail(ErrorCode::SampleError, error);
    }
    if (loaded.attack.empty() || loaded.body.empty()) {
      return Fail(ErrorCode::SampleError,
                  "sample must contain an attack and BODY");
    }
    return Ok();
  }

  if (sample.attack_file.empty() || sample.body_file.empty()) {
    return Fail(ErrorCode::InvalidArgument,
                "both attack_file and body_file are required");
  }
  std::vector<int16_t> body;
  if (!cardlink::audio::BodyWithHeadOverlap(
          sample.attack_file, sample.body_file, body, error)) {
    return Fail(ErrorCode::SampleError, error);
  }
  return body.empty() ? Fail(ErrorCode::SampleError, "BODY is empty") : Ok();
}

} // namespace

using detail::Fail;
using detail::FromCard;
using detail::Ok;

Result Core::Impl::RefreshAttackSamples()
{
  cardproto::Result reply;
  {
    std::lock_guard<std::mutex> lock(bus_mutex);
    reply = bus.Channel().Exec("a");
  }
  if (!reply.ok()) {
    return FromCard(reply);
  }

  unsigned count = 0;
  char mask[65]{};
  if (std::sscanf(reply.raw, "ok: a %u %64s", &count, mask) != 2 ||
      std::char_traits<char>::length(mask) != 64u || count > 256u) {
    return Fail(ErrorCode::BadReply, "invalid Channel sample status");
  }

  unsigned parsed_count = 0;
  for (size_t byte = 0; byte < 32u; ++byte) {
    const int high = HexNibble(mask[byte * 2u]);
    const int low = HexNibble(mask[byte * 2u + 1u]);
    if (high < 0 || low < 0) {
      return Fail(ErrorCode::BadReply, "invalid Channel sample status");
    }
    const uint8_t bits = static_cast<uint8_t>((high << 4) | low);
    for (size_t bit = 0; bit < 8u; ++bit) {
      const bool loaded = (bits & static_cast<uint8_t>(1u << bit)) != 0u;
      attack_samples[byte * 8u + bit] = loaded;
      parsed_count += loaded ? 1u : 0u;
    }
  }
  if (parsed_count != count) {
    return Fail(ErrorCode::BadReply, "inconsistent Channel sample status");
  }
  return Ok();
}

Result Core::Impl::LoadSample(const SampleDefinition &sample)
{
  std::string error;
  bool loaded = false;
  last_sample_result = Ok();
  if (!sample.sample_file.empty()) {
    loaded = samples.LoadWave(sample.id, sample.sample_file,
                              sample.raw_sample_rate_hz, error);
  } else {
    loaded = samples.LoadHead(sample.id, sample.attack_file, error) &&
             samples.LoadBody(sample.id, sample.body_file, error);
  }
  if (!loaded) {
    (void)RefreshAttackSamples();
    loaded_samples[sample.id] = false;
    return Fail(ErrorCode::SampleError, error);
  }
  attack_samples[sample.id] = true;
  if (!samples.SetRootHz(sample.id, sample.root_hz, error)) {
    loaded_samples[sample.id] = false;
    return Fail(ErrorCode::SampleError, error);
  }
  loaded_samples[sample.id] = true;
  return Ok("sample " + std::to_string(sample.id) + " loaded");
}

Result Core::loadSample(const SampleDefinition &sample)
{
  const Result validation = ValidateSample(sample);
  if (!validation) {
    return validation;
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  const Result idle = impl_->SilenceAndWaitForIdle();
  return idle ? impl_->LoadSample(sample) : idle;
}

Result Core::loadSampleBank(const std::vector<SampleDefinition> &samples)
{
  if (samples.empty()) {
    return Fail(ErrorCode::InvalidArgument, "sample bank is empty");
  }
  std::set<uint16_t> ids;
  for (const auto &sample : samples) {
    const Result validation = ValidateSample(sample);
    if (!validation) {
      return validation;
    }
    if (!ids.insert(sample.id).second) {
      return Fail(ErrorCode::InvalidArgument,
                  "sample bank contains duplicate id " +
                      std::to_string(sample.id));
    }
  }

  std::vector<SampleDefinition> ordered = samples;
  std::sort(ordered.begin(), ordered.end(),
            [](const SampleDefinition &left, const SampleDefinition &right) {
              return left.id < right.id;
            });

  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  Result result = impl_->SilenceAndWaitForIdle();
  if (!result) {
    return result;
  }
  for (const auto &sample : ordered) {
    result = impl_->LoadSample(sample);
    if (!result) {
      return result;
    }
  }
  return Ok("sample bank loaded");
}

Result Core::loadSampleFolder(const std::string &path)
{
  if (path.empty() || !std::filesystem::is_directory(path)) {
    return Fail(ErrorCode::InvalidArgument, "sample folder does not exist");
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  const Result idle = impl_->SilenceAndWaitForIdle();
  if (!idle) {
    return idle;
  }
  std::string error;
  const int loaded = impl_->samples.LoadFolder(path, error);
  (void)impl_->RefreshAttackSamples();
  for (size_t id = 0; id < impl_->loaded_samples.size(); ++id) {
    impl_->loaded_samples[id] =
        impl_->samples.Mixer().HasBody(static_cast<uint16_t>(id)) &&
        impl_->attack_samples[id];
  }
  if (loaded <= 0) {
    return Fail(ErrorCode::SampleError,
                error.empty() ? "sample folder is empty" : error);
  }
  return Ok("loaded " + std::to_string(loaded) + " samples");
}

Result Core::setSampleRoot(uint16_t sample_id, double root_hz)
{
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  const Result connection = impl_->RequireConnected();
  if (!connection) {
    return connection;
  }
  if (sample_id >= 256u || !(root_hz > 0.0)) {
    return Fail(ErrorCode::InvalidArgument,
                "sample id must be 0..255 and root_hz must be positive");
  }
  std::string error;
  return impl_->samples.SetRootHz(sample_id, root_hz, error)
             ? Ok("sample root updated")
             : Fail(ErrorCode::SampleError, error);
}

} // namespace cmi
