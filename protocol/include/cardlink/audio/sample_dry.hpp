/**
 * @file sample_dry.hpp
 * @brief Host body feeder: unpitched int16 → vendor bulk BODY bursts.
 *
 * Card owns pitch / env / filter. Host sends to the voice with the least
 * remaining buffer time. `vq` free-slots + note/root give that time; the
 * card's `best` is the same score from its playhead. Send while remaining
 * time is below kStopMs and the ring has room. First SOF burst does not
 * wait for a poll.
 *
 * Fill is USB-accepted BODY minus consume. `vq` does not overwrite it.
 * Stop if the card already has kStopMs of ring, or free-slot code 0.
 * Burst size is at most the last `vq` free samples (minus BODY sent
 * since that poll). USB NAK when the vendor FIFO is full; a full ring
 * drops the whole burst.
 *
 * `queued` tracks the host file cursor (attack does not consume body).
 *
 * The bulk thread posts no mutex: UI posts to a SPSC command queue.
 */

#ifndef CARDLINK_AUDIO_SAMPLE_DRY_HPP
#define CARDLINK_AUDIO_SAMPLE_DRY_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cardlink {
namespace audio {

constexpr unsigned kSampleVoices = 8;
constexpr unsigned kAttackWaves = 256;
constexpr unsigned kSampleRateHz = 48000;
constexpr unsigned kAttackSamples = 512;
constexpr unsigned kCrossfadeSamples = 32;
constexpr unsigned kBodyOrigin = kAttackSamples - kCrossfadeSamples;
constexpr unsigned kRingSamples = 4096;
/** Leave room for interpolator taps. */
constexpr unsigned kRingHeadroom = 32;
/** vq free-slot code: 0..14 complete 256-sample slots; 15 = empty. */
constexpr unsigned kVqSlotSamples = 256;
constexpr unsigned kVqSlotMax = 14;
constexpr unsigned kVqSlotEmpty = 15;
/** Refill if the ring holds less than this of play time (poll + a few FS frames). */
constexpr unsigned kNeedMs = 8;
/** Stop sending once the ring holds this much play time (pitch-aware). */
constexpr unsigned kStopMs = 25;
/** Do not emit a BODY header for a handful of samples unless starving. */
constexpr unsigned kMinBurst = 32;
constexpr unsigned kStreamSessionMod = 7;
constexpr unsigned kBodyBurstMax = 512;
constexpr double kDefaultBodyRootHz = 261.625565;

class SampleDryMixer {
public:
  SampleDryMixer();

  bool LoadBodyFile(uint16_t wave_id, const std::string &path, std::string &err);

  /** Replace body samples (48 kHz int16, from head_len − overlap).
   *  Rejected while a voice is playing this id (the bulk thread reads the table). */
  bool SetBody(uint16_t wave_id, const int16_t *data, size_t nsamp,
               std::string &err);

  bool HasBody(uint16_t wave_id) const;

  /** Committed AXI head length. 0 = consume body from note-on. */
  void SetAttackLen(uint16_t wave_id, unsigned nsamp);
  unsigned AttackLen(uint16_t wave_id) const;

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

  /** Wave id the bulk thread is streaming for this voice, or 0xFFFF if idle. */
  uint16_t LiveWave(uint8_t voice) const;

  /** Subtract body consume for `nframes` of 48 kHz output (attack does not). */
  void ConsumeOutputSamples(double nframes);

  /** Samples this voice can take now (0 if inactive / enough time / full). */
  unsigned WantBurst(uint8_t voice) const;

  /** Active voice with the least remaining time that still wants a burst.
   *  0xFF if none. Prefers the card's vq `best` when that voice wants data. */
  uint8_t HungriestWant() const;

  /** Copy up to max_n body samples. Sets sof/session. */
  unsigned FillBurst(uint8_t voice, int16_t *dst, unsigned max_n, bool &sof,
                     uint8_t &session);

  /** Undo FillBurst when the bulk OUT did not accept the packet. */
  void AbortBurst(uint8_t voice, unsigned nsamp, bool sof);

  /** RS485/CDC `vq`: mask, hungriest, per-voice free-slot codes 0..15. */
  void ApplyVoiceQuery(uint8_t mask, uint8_t best, const uint8_t *free_slots);

  void SetPitchHz(uint8_t voice, double freq_hz);

  unsigned QueuedSamples(uint8_t voice) const;

  /** True once at least one BODY burst is queued. */
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
    double phase = 0.0;
    double queued = 0.0;
    double fill = 0.0;
    double fill_at_vq = 0.0;
    unsigned attack_len = 0;
    uint32_t vq_seen = 0;
    unsigned vq_free = 0;
    bool vq_have = false;
  };

  static constexpr unsigned kCmdCap = 32;

  void Post(const Cmd &c);
  void DrainCmds();
  void ApplyCmd(const Cmd &c);
  int16_t NextBody(Voice &v);
  double IncOf(const Voice &v) const;
  static double Fade0Of(unsigned attack_len);
  /** True once the card interpolator has reached the attack join. */
  static bool BodyDraining(const Voice &v);
  void PullVq();
  double RemainingMs(const Voice &v) const;
  unsigned TargetFill(const Voice &v) const;
  unsigned VqRoom(const Voice &v) const;
  bool WaveInUse(uint16_t wave_id) const;

  std::array<Cmd, kCmdCap> cmds_{};
  std::atomic<uint32_t> cmd_wr_{0};
  std::atomic<uint32_t> cmd_rd_{0};

  std::array<Voice, kSampleVoices> voices_{};
  std::array<std::atomic<unsigned>, kSampleVoices> sent_{};
  std::array<std::atomic<bool>, kSampleVoices> live_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> live_wave_{};
  std::atomic<uint32_t> vq_seq_{1};
  std::atomic<uint8_t> vq_mask_{0};
  std::atomic<uint8_t> vq_best_{0xFF};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_fill_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_free_{};

  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<std::atomic<double>, 256> root_hz_{};
  std::array<std::atomic<bool>, 256> oneshot_{};
  std::array<std::atomic<unsigned>, 256> attack_len_{};
};

} // namespace audio
} // namespace cardlink

#endif /* CARDLINK_AUDIO_SAMPLE_DRY_HPP */
