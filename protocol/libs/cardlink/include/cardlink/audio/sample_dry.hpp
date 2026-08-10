/**
 * @file sample_dry.hpp
 * @brief Host-side dry SAMPLE mixer: 8 int16 body streams → interleaved UAC.
 *
 * No envelope/filter — card owns DSP. Bodies are resampled to MIDI pitch
 * (freq_hz / root_hz). Thread-safe note on/off + Render.
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
/** Default root when a body has no roots.txt entry (middle C). */
constexpr double kDefaultBodyRootHz = 261.625565;

class SampleDryMixer {
public:
  SampleDryMixer();

  /** Load int16 LE body file for wave_id. Root stays previous/default. */
  bool LoadBodyFile(uint16_t wave_id, const std::string &path, std::string &err);

  /**
   * Set native pitch of a loaded body (Hz of the recording).
   * Playback rate = note_freq_hz / root_hz.
   */
  void SetBodyRootHz(uint16_t wave_id, double root_hz);

  double BodyRootHz(uint16_t wave_id) const;

  /**
   * Load `roots.txt` lines: `<wave_id> <root_hz> [loop|oneshot]`
   * (comments with #). Missing mode defaults to loop.
   * Returns false only on I/O hard fail; missing file is ok (false + err).
   */
  bool LoadRootsFile(const std::string &path, std::string &err);

  /** If true, Render holds silence after the last body sample (no wrap). */
  void SetBodyOneshot(uint16_t wave_id, bool oneshot);

  bool BodyOneshot(uint16_t wave_id) const;

  /** Arm body stream; pitch from freq_hz vs body root. */
  void NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz);

  /**
   * Key-up. Does **not** stop the dry stream: the card still needs body
   * samples while NoteEnv runs release. Call Silence() when the card
   * reports the voice idle (vq poll).
   */
  void NoteOff(uint8_t voice);

  /** Stop rendering one voice (card finished release / idle). */
  void Silence(uint8_t voice);

  void AllNotesOff();

  /** True if any voice is still rendering dry body into UAC. */
  bool AnyActive() const;

  /**
   * Fill interleaved int16 frames: ch0..ch7 per frame.
   * Loop bodies wrap; oneshot bodies hold 0 after the end (card may still
   * need dry frames through NoteEnv release until vq idle → Silence).
   */
  void Render(int16_t *interleaved, unsigned nframes);

private:
  struct Voice {
    bool active = false;
    uint16_t wave_id = 0;
    double phase = 0.0;     /**< fractional sample index into body */
    double phase_inc = 1.0; /**< samples of body per output sample */
  };

  mutable std::mutex mu_;
  std::array<Voice, kSampleVoices> voices_{};
  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<double, 256> root_hz_{};
  std::array<bool, 256> oneshot_{};
};

} // namespace audio
} // namespace cardlink

#endif /* HOST_IO_AUDIO_SAMPLE_DRY_HPP */
