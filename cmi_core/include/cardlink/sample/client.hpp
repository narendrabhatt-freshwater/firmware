/**
 * @file client.hpp
 * @brief Plug-and-play Channel SAMPLE session for host apps.
 *
 * Owns wave split, CDC attack upload, BODY mixer, and note commands.
 * The UI supplies a console sink (RS485 or CDC ASCII). Attack heads are
 * sample wave_id 0..247; IDs 248..255 are reserved wavetables. MIDI NoteOn
 * maps key → wave_id. Body streaming is SampleBulkOut, not this class.
 */

#ifndef CARDLINK_SAMPLE_CLIENT_HPP
#define CARDLINK_SAMPLE_CLIENT_HPP

#include "cardlink/audio/sample_dry.hpp"
#include "cardlink/serial_port.hpp"

#include <array>
#include <cstddef>
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

struct NoteRequest {
  uint8_t voice = 0u;
  uint8_t key = 0u;
  uint8_t velocity = 127u;
  uint16_t wave_id = 0xFFFFu;
  uint8_t session = 0xFFu;
  bool note_on = true;
};

class Client {
public:
  /** Bare Channel command, e.g. `n3 on 60 127` — no `c:` prefix. */
  using ConsoleFn = std::function<void(const std::string &cmd)>;
  using NoteGateStart = std::function<void()>;
  using NoteGateDone = std::function<void(bool applied)>;
  /** Queue one RS485 aw/nX transaction. The worker invokes start immediately
   * before aw/nX and done after the transaction completes. */
  using NoteGateFn =
      std::function<bool(const NoteRequest &note, NoteGateStart start,
                         NoteGateDone done)>;

  void SetConsole(ConsoleFn fn);
  void SetNoteGate(NoteGateFn fn);
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
  bool LoadWave(uint16_t wave_id, const std::string &path,
                uint32_t raw_rate_hz, std::string &err);
  bool LoadHead(uint16_t wave_id, const std::string &path, std::string &err);
  bool LoadBody(uint16_t wave_id, const std::string &path, std::string &err);
  /** Upload logical oscillator wave 0..7; firmware chooses bank placement. */
  bool LoadWavetable(uint8_t wave, const std::string &path, std::string &err);
  /** wN_*_head.i8 + wN_*_body.i8, or matching stems. Optional roots.txt. */
  int LoadFolder(const std::string &dir, std::string &err);

  bool SetRootHz(uint16_t wave_id, double hz, std::string &err);

  /** Reserve/launch matching USB SOF, then queue authoritative RS485 aw/nX. */
  void NoteOn(uint8_t voice, uint8_t key, uint16_t wave_id = 0xFFFFu,
              uint8_t velocity = 127u);
  /** Queue RS485 note starts; each worker launches its matching BODY session
   *  immediately before aw/nX so the attack bridges into the first samples. */
  bool NoteOnBatch(const NoteRequest *notes, size_t count);
  void NoteOff(uint8_t voice);
  void AllNotesOff();
  void Silence(uint8_t voice);

  const Slot &GetSlot(uint8_t voice) const;

private:
  bool UploadAttack(uint16_t wave_id, const int8_t *pcm, size_t nsamp,
                    std::string &err);
  bool UploadAttackFile(uint16_t wave_id, const std::string &path,
                        std::string &err);
  bool SendCdcLine(const std::string &line, std::string &err);
  void SendConsole(const std::string &cmd);
  void SetLabel(uint16_t wave_id);

  ConsoleFn console_;
  NoteGateFn note_gate_;
  std::string cdc_path_;
  cardlink::SerialPort cdc_port_;
  int cdc_refs_ = 0;
  cardlink::audio::SampleDryMixer mixer_;
  std::array<Slot, cardlink::audio::kSampleVoices> slots_{};
};

} // namespace sample
} // namespace cardlink

#endif /* CARDLINK_SAMPLE_CLIENT_HPP */
