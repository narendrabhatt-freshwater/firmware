/**
 * @file sample_dry.hpp
 * @brief Host body feeder: unpitched int16 → tagged UAC frames.
 *
 * Card owns pitch / env / filter. Each UAC frame: ch0 = route, ch1..7 = samples
 * for one voice (hungriest). Prefill before nX: SOF then ≥kPrefillSamples.
 */

#ifndef HOST_IO_AUDIO_SAMPLE_DRY_HPP
#define HOST_IO_AUDIO_SAMPLE_DRY_HPP

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cardlink {
namespace audio {

constexpr unsigned kSampleVoices = 8;
constexpr unsigned kSampleRateHz = 48000;
constexpr unsigned kAttackSamples = 256;
constexpr unsigned kCrossfadeSamples = 32;
constexpr unsigned kBodyOrigin = kAttackSamples - kCrossfadeSamples;
constexpr unsigned kPrefillSamples = 256;
constexpr uint16_t kUacTagBase = 0x7F00;
constexpr uint8_t kUacIdle = 0xFF;
constexpr uint8_t kUacSof = 0x10;
constexpr unsigned kRingSamples = 2048;
/** Default root when a body has no roots.txt entry (middle C). */
constexpr double kDefaultBodyRootHz = 261.625565;

class SampleDryMixer {
public:
  SampleDryMixer();

  bool LoadBodyFile(uint16_t wave_id, const std::string &path, std::string &err);

  /** Replace body samples (already 48 kHz int16, starts at file sample 224). */
  bool SetBody(uint16_t wave_id, const int16_t *data, size_t nsamp,
               std::string &err);

  void SetBodyRootHz(uint16_t wave_id, double root_hz);
  double BodyRootHz(uint16_t wave_id) const;

  bool LoadRootsFile(const std::string &path, std::string &err);

  void SetBodyOneshot(uint16_t wave_id, bool oneshot);
  bool BodyOneshot(uint16_t wave_id) const;

  /** Arm unpitched cursor at 0; next Render frames SOF+prefill this voice. */
  void NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz);

  void NoteOff(uint8_t voice);
  void Silence(uint8_t voice);
  void AllNotesOff();

  bool AnyActive() const;

  /**
   * Fill interleaved int16 UAC frames (8 ch). ch0 = route, ch1..7 = body.
   */
  void Render(int16_t *interleaved, unsigned nframes);

  /** Host estimate of queued-minus-consumed source samples (mux). */
  void SetPitchHz(uint8_t voice, double freq_hz);

private:
  struct Voice {
    bool active = false;
    bool sof_pending = false;
    uint16_t wave_id = 0;
    double freq_hz = 0.0;
    double cursor = 0.0;     /**< next body sample to send */
    double queued = 0.0;     /**< source samples delivered this note */
    double consumed = 0.0;   /**< estimated card consume (inc per frame) */
  };

  uint8_t PickVoiceLocked();
  int16_t NextBodyLocked(Voice &v);
  double IncLocked(const Voice &v) const;

  mutable std::mutex mu_;
  std::array<Voice, kSampleVoices> voices_{};
  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<double, 256> root_hz_{};
  std::array<bool, 256> oneshot_{};
  uint8_t rr_ = 0;
};

} // namespace audio
} // namespace cardlink

#endif /* HOST_IO_AUDIO_SAMPLE_DRY_HPP */
