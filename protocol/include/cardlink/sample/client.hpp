/**
 * @file client.hpp
 * @brief Plug-and-play Channel SAMPLE session for host apps.
 *
 * Owns wave split, CDC attack upload, BODY mixer, and note commands.
 * The UI supplies a console sink (RS485 or CDC ASCII). Attack heads are
 * wave_id 0..255; eight voices assign any loaded head (`aw`). MIDI NoteOn
 * maps key → wave_id. Body streaming is SampleBulkOut, not this class.
 */

#ifndef CARDLINK_SAMPLE_CLIENT_HPP
#define CARDLINK_SAMPLE_CLIENT_HPP

#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/serial_port.hpp"

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

  /**
   * Hold the Channel Card CDC port open across a burst of LoadWave / SetRootHz.
   * Pair each successful BeginCdc with EndCdc. Nested holds are refcounted.
   */
  bool BeginCdc(std::string &err);
  void EndCdc();

  cardlink::audio::SampleDryMixer &Mixer() { return mixer_; }
  const cardlink::audio::SampleDryMixer &Mixer() const { return mixer_; }

  bool LoadWave(uint16_t wave_id, const std::string &path, std::string &err);
  bool LoadHead(uint16_t wave_id, const std::string &path, std::string &err);
  bool LoadBody(uint16_t wave_id, const std::string &path, std::string &err);
  /** wN_*_head.i32/.i16 + wN_*_body.i16, or matching stems. Optional roots.txt. */
  int LoadFolder(const std::string &dir, std::string &err);

  bool SetRootHz(uint16_t wave_id, double hz, std::string &err);

  /** Mixer + `aw` + console `nX`. Does not wait for USB prefill.
   *  wave_id 0xFFFF: use voice, or the first loaded body if that slot is empty. */
  void NoteOn(uint8_t voice, double hz, uint16_t wave_id = 0xFFFFu);
  void NoteOff(uint8_t voice);
  void AllNotesOff();
  void Silence(uint8_t voice);

  const Slot &GetSlot(uint8_t voice) const;

private:
  bool UploadAttack(uint16_t wave_id, const int16_t *q15, size_t nsamp,
                    std::string &err);
  bool UploadAttackFile(uint16_t wave_id, const std::string &path,
                        std::string &err);
  bool SendCdcLine(const std::string &line, std::string &err);
  void SendConsole(const std::string &cmd);
  void SetLabel(uint16_t wave_id);

  ConsoleFn console_;
  std::string cdc_path_;
  cardlink::SerialPort cdc_port_;
  int cdc_refs_ = 0;
  cardlink::audio::SampleDryMixer mixer_;
  std::array<Slot, cardlink::audio::kSampleVoices> slots_{};
};

} // namespace sample
} // namespace cardlink

#endif /* CARDLINK_SAMPLE_CLIENT_HPP */
