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
  double hz = 0.0;
  uint16_t wave_id = 0xFFFFu;
  uint8_t session = 0xFFu;
};

class Client {
public:
  /** Bare Channel command, e.g. `n3 261.63` — no `c:` prefix. */
  using ConsoleFn = std::function<void(const std::string &cmd)>;
  using NoteGateDone = std::function<void(bool applied)>;
  /** Queue one RS485 aw/nX transaction. done(false) cancels its launched SOF. */
  using NoteGateFn =
      std::function<bool(const NoteRequest &note, NoteGateDone done)>;

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
  bool LoadHead(uint16_t wave_id, const std::string &path, std::string &err);
  bool LoadBody(uint16_t wave_id, const std::string &path, std::string &err);
  /** wN_*_head.i32/.i16 + wN_*_body.i16, or matching stems. Optional roots.txt. */
  int LoadFolder(const std::string &dir, std::string &err);

  bool SetRootHz(uint16_t wave_id, double hz, std::string &err);

  /** Reserve/launch matching USB SOF, then queue authoritative RS485 aw/nX. */
  void NoteOn(uint8_t voice, double hz, uint16_t wave_id = 0xFFFFu);
  /** Launch newest-first BODY sessions, then queue their RS485 note starts.
   *  The card's attack heads bridge into the first BODY samples.
   *  The timeout argument remains for source/ABI compatibility and is ignored. */
  bool NoteOnBatch(const NoteRequest *notes, size_t count,
                   unsigned timeout_ms = 200u);
  void NoteOff(uint8_t voice);
  void AllNotesOff();
  void Silence(uint8_t voice);

  /** Configure 0..50 ms release before a stolen slot retriggers. */
  void SetCrashReleaseMs(uint8_t release_ms);
  uint8_t CrashReleaseMs() const { return crash_release_ms_; }

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
  NoteGateFn note_gate_;
  std::string cdc_path_;
  cardlink::SerialPort cdc_port_;
  int cdc_refs_ = 0;
  uint8_t crash_release_ms_ = 3u;
  cardlink::audio::SampleDryMixer mixer_;
  std::array<Slot, cardlink::audio::kSampleVoices> slots_{};
};

} // namespace sample
} // namespace cardlink

#endif /* CARDLINK_SAMPLE_CLIENT_HPP */
