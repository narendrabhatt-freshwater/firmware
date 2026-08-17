/**
 * @file client.hpp
 * @brief Plug-and-play Channel SAMPLE session for host apps.
 *
 * Owns wave split, CDC attack upload, UAC body mixer, and note commands.
 * The UI supplies a console sink (RS485 or CDC ASCII) and calls Render()
 * from the UAC callback. Voice N owns attack slot N and body table N.
 */

#ifndef CARDLINK_SAMPLE_CLIENT_HPP
#define CARDLINK_SAMPLE_CLIENT_HPP

#include "cardlink/audio/sample_dry.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace cardlink {
namespace sample {

struct Slot {
  std::string head_path;
  std::string body_path;
  std::string label;
  bool head_on_card = false;
  bool body_ready = false;
};

class Client {
public:
  /** Bare Channel command, e.g. `n3 261.63` — no `c:` prefix. */
  using ConsoleFn = std::function<void(const std::string &cmd)>;

  void SetConsole(ConsoleFn fn);
  void SetCdcPath(const std::string &path);
  const std::string &CdcPath() const { return cdc_path_; }

  cardlink::audio::SampleDryMixer &Mixer() { return mixer_; }
  const cardlink::audio::SampleDryMixer &Mixer() const { return mixer_; }

  /** UAC callback — 10ch int16 interleaved. No mutex. */
  void Render(int16_t *interleaved, unsigned nframes);

  bool LoadWave(uint8_t voice, const std::string &path, std::string &err);
  bool LoadHead(uint8_t voice, const std::string &path, std::string &err);
  bool LoadBody(uint8_t voice, const std::string &path, std::string &err);
  /** wN_*_head.i32 + wN_*_body.i16, or matching stems. Optional roots.txt. */
  int LoadFolder(const std::string &dir, std::string &err);

  bool SetRootHz(uint8_t voice, double hz, std::string &err);

  /** Mixer + console `nX`. Does not wait for USB prefill. */
  void NoteOn(uint8_t voice, double hz);
  void NoteOff(uint8_t voice);
  void AllNotesOff();
  void Silence(uint8_t voice);

  const Slot &GetSlot(uint8_t voice) const;

private:
  bool UploadAttack(uint8_t voice, const int32_t *q31, size_t nsamp,
                    std::string &err);
  bool UploadAttackFile(uint8_t voice, const std::string &path,
                        std::string &err);
  bool SendCdcLine(const std::string &line, std::string &err);
  void SendConsole(const std::string &cmd);
  void SetLabel(uint8_t voice);

  ConsoleFn console_;
  std::string cdc_path_;
  cardlink::audio::SampleDryMixer mixer_;
  std::array<Slot, cardlink::audio::kSampleVoices> slots_{};
};

} // namespace sample
} // namespace cardlink

#endif /* CARDLINK_SAMPLE_CLIENT_HPP */
