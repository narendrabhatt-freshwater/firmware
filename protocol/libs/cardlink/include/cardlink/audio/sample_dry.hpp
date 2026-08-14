/**
 * @file sample_dry.hpp
 * @brief Host body feeder: unpitched int16 → tagged UAC frames.
 *
 * Card owns pitch / env / filter. Each UAC frame: ch0 = route, ch1..9 =
 * samples for one voice. Mux is a credit scheduler: each output frame
 * adds phase_inc credits; a sent frame costs 9.
 *
 * Render() is the UAC callback — no mutex. UI posts to a SPSC command
 * queue; Render drains it at the start of the buffer.
 */

#ifndef HOST_IO_AUDIO_SAMPLE_DRY_HPP
#define HOST_IO_AUDIO_SAMPLE_DRY_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cardlink {
namespace audio {

constexpr unsigned kSampleVoices = 8;
constexpr unsigned kSampleRateHz = 48000;
constexpr unsigned kAttackSamples = 8192;
constexpr unsigned kCrossfadeSamples = 32;
constexpr unsigned kBodyOrigin = kAttackSamples - kCrossfadeSamples;
/** Hungry-voice boost until this many body samples are queued. Not a wait. */
constexpr unsigned kPrefillSamples = 256;
constexpr uint16_t kUacTagBase = 0x7F00;
constexpr uint8_t kUacIdle = 0xFF;
constexpr uint8_t kUacSof = 0x10;
constexpr unsigned kUacSessionShift = 5;
constexpr unsigned kUacSessionMod = 7;
constexpr unsigned kRingSamples = 2048;
constexpr unsigned kUacChannels = 10;
constexpr unsigned kUacBodyPerFrame = kUacChannels - 1u;
constexpr double kDefaultBodyRootHz = 261.625565;

class SampleDryMixer {
public:
  SampleDryMixer();

  bool LoadBodyFile(uint16_t wave_id, const std::string &path, std::string &err);

  /** Replace body samples (48 kHz int16, starts at kBodyOrigin).
   *  Rejected while a voice is playing this id (callback reads the table). */
  bool SetBody(uint16_t wave_id, const int16_t *data, size_t nsamp,
               std::string &err);

  void SetBodyRootHz(uint16_t wave_id, double root_hz);
  double BodyRootHz(uint16_t wave_id) const;

  bool LoadRootsFile(const std::string &path, std::string &err);

  void SetBodyOneshot(uint16_t wave_id, bool oneshot);
  bool BodyOneshot(uint16_t wave_id) const;

  /** New session: SOF + prefill this voice from cursor 0. */
  void NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz);

  /** Key-up: card releases. Body stream continues until Silence. */
  void NoteOff(uint8_t voice);
  void Silence(uint8_t voice);
  void AllNotesOff();

  bool AnyActive() const;

  void Render(int16_t *interleaved, unsigned nframes);

  void ApplyVoiceQuery(uint8_t mask, uint8_t best, const uint8_t *free_slots);

  void SetPitchHz(uint8_t voice, double freq_hz);

  unsigned QueuedSamples(uint8_t voice) const;

  bool WaitPrefill(uint8_t voice, unsigned timeout_ms);
  bool WaitPrefillActive(unsigned timeout_ms);

private:
  enum class CmdKind : uint8_t { On, Pitch, Silence, AllOff };

  struct Cmd {
    CmdKind kind = CmdKind::On;
    uint8_t voice = 0;
    uint16_t wave_id = 0;
    double freq_hz = 0.0;
  };

  struct Voice {
    bool active = false;
    bool sof_pending = false;
    uint8_t session = 0;
    uint16_t wave_id = 0;
    double freq_hz = 0.0;
    double cursor = 0.0;
    double credit = 0.0;
  };

  static constexpr unsigned kCmdCap = 32;

  void Post(const Cmd &c);
  void DrainCmds();
  void ApplyCmd(const Cmd &c);
  uint8_t PickVoice() const;
  int16_t NextBody(Voice &v);
  double IncOf(const Voice &v) const;
  static int16_t EncodeTag(uint8_t session, uint8_t route_low);
  bool WaveInUse(uint16_t wave_id) const;

  std::array<Cmd, kCmdCap> cmds_{};
  std::atomic<uint32_t> cmd_wr_{0};
  std::atomic<uint32_t> cmd_rd_{0};

  std::array<Voice, kSampleVoices> voices_{};
  std::array<std::atomic<unsigned>, kSampleVoices> sent_{};
  std::array<std::atomic<bool>, kSampleVoices> live_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> live_wave_{};

  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<std::atomic<double>, 256> root_hz_{};
  std::array<std::atomic<bool>, 256> oneshot_{};
};

} // namespace audio
} // namespace cardlink

#endif /* HOST_IO_AUDIO_SAMPLE_DRY_HPP */
