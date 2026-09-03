#include "cardlink/audio/wave_loader.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/usb/stream_proto.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <type_traits>
#include <vector>

namespace {

void Expect(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void Write(const std::filesystem::path &path, const std::vector<uint8_t> &data)
{
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(data.data()),
            static_cast<std::streamsize>(data.size()));
  Expect(out.good(), "test asset write failed");
}

} // namespace

int main()
{
  using cardlink::audio::LoadedWave;
  static_assert(std::is_same_v<decltype(LoadedWave::attack)::value_type,
                               int8_t>);
  static_assert(cardlink::usb::kStreamUacChannels == 21u);
  static_assert(cardlink::usb::kStreamUacRateHz == 48000u);
  static_assert(cardlink::usb::kStreamUacPacketBytes == 1008u);
  static_assert(cardlink::usb::kStreamUacBodySamples == 1004u);

  const auto dir = std::filesystem::temp_directory_path() /
                   "cmi_core_sample_format_test";
  std::filesystem::create_directories(dir);
  const auto raw = dir / "extremes.raw";
  const auto wav = dir / "unsigned8.wav";
  const auto head = dir / "head.i8";
  const auto body = dir / "body.i8";
  Write(raw, {0x80u, 0x00u, 0xdau, 0x7fu});
  Write(wav, {
      'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
      0x80, 0xbb, 0x00, 0x00, 0x80, 0xbb, 0x00, 0x00,
      1, 0, 8, 0, 'd', 'a', 't', 'a', 4, 0, 0, 0,
      0x00, 0x80, 0xff, 0x40});
  Write(head, {0x80u, 0xffu, 0x00u, 0x7fu});
  Write(body, {0x05u, 0xfbu});

  std::string error;
  LoadedWave wave;
  Expect(cardlink::audio::LoadWaveFile(raw.string(), 48000u, wave, error),
         "signed-int8 raw must load");
  Expect(wave.attack.size() == 4u && wave.attack[0] == -128 &&
             wave.attack[1] == 0 && wave.attack[2] == -38 &&
             wave.attack[3] == 127,
         "signed-int8 raw must round-trip exactly at 48 kHz");

  LoadedWave wav_wave;
  Expect(cardlink::audio::LoadWaveFile(wav.string(), 0u, wav_wave, error),
         "standard unsigned 8-bit WAV must load");
  Expect(wav_wave.attack.size() == 4u && wav_wave.attack[0] == -128 &&
             wav_wave.attack[1] == 0 && wav_wave.attack[2] == 127 &&
             wav_wave.attack[3] == -64,
         "unsigned 8-bit WAV must convert to canonical signed int8");

  std::vector<int8_t> joined;
  Expect(cardlink::audio::BodyWithHeadOverlap(
             head.string(), body.string(), joined, error),
         "signed-int8 split assets must load");
  Expect(joined.size() == 6u && joined[0] == -128 && joined[3] == 127 &&
             joined[4] == 5 && joined[5] == -5,
         "split BODY must begin with the signed-int8 head overlap");

  cardlink::sample::Client client;
  Expect(!client.LoadHead(0u, (dir / "legacy.i16").string(), error) &&
             error.find(".i8") != std::string::npos,
         "legacy split attack formats must be rejected");

  std::filesystem::remove_all(dir);
  return 0;
}
