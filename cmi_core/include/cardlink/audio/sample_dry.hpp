/**
 * @file sample_dry.hpp
 * @brief Host body feeder: unpitched int16 → direct UAC2 BODY frames.
 *
 * Card owns pitch / env / filter. The host binds one session to nX and waits
 * for matching ABI6 `vq` authority before emitting BODY. SOF repeats until a
 * later status confirms that session and its first complete BODY frame.
 * Thereafter each status grants exact runtime-capacity credit.
 * Every 1 ms UAC packet carries routing/sequence metadata and 508 raw samples.
 * Packets are assigned by source consumption rate so a fast voice cannot
 * consume another voice's share before the next status.
 *
 * `queued` estimates card FIFO occupancy (attack does not consume body).
 *
 * UI and bus-worker note changes reach the BODY thread through a bounded
 * producer-serialized command queue. The audio consumer remains lock-free.
 */

#ifndef CARDLINK_AUDIO_SAMPLE_DRY_HPP
#define CARDLINK_AUDIO_SAMPLE_DRY_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "cardproto/channel.hpp"

namespace cardlink {
namespace audio {

constexpr unsigned kSampleVoices = 8;
constexpr unsigned kAttackWaves = 256;
constexpr unsigned kSampleRateHz = 48000;
constexpr unsigned kAttackSamples = 512;
constexpr unsigned kCrossfadeSamples = 32;
constexpr unsigned kBodyOrigin = kAttackSamples - kCrossfadeSamples;
/** Leave room for interpolator taps. */
constexpr unsigned kRingHeadroom = 32;
/** Coalesce refill credit to amortize eight per-voice BODY metas. */
constexpr unsigned kMinBurst = 512;
constexpr unsigned kStreamSessionMod = 255;
/** One BODY meta remains bounded; the larger ring absorbs poll/host jitter. */
constexpr unsigned kBodyBurstMax = 4096;
constexpr double kDefaultBodyRootHz = 261.625565;

class SampleDryMixer {
public:
  SampleDryMixer();

  bool LoadBodyFile(uint16_t wave_id, const std::string &path, std::string &err);

  /** Replace body samples (48 kHz int16, from head_len − overlap).
   *  Rejected while a voice is playing this id (the BODY thread reads the table). */
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

  /** Queue a new ready-gated note session. Returns its synchronous session id. */
  uint8_t NoteOn(uint8_t voice, uint16_t wave_id,
                 double attack_elapsed_ms = 0.0);

  /** Allocate the identity that must be carried by both nX and USB SOF. */
  uint8_t ReserveSession(uint8_t voice);

  /** Queue a note using a session already bound into authoritative nX.
   * source_hz is a transport scheduling hint only; the card/script remains
   * authoritative for the actual mapped pitch. */
  uint8_t NoteOnSession(uint8_t voice, uint16_t wave_id, uint8_t session,
                        double attack_elapsed_ms = 0.0,
                        double source_hz = 0.0);

  /** Cancel a pre-authority session without touching a newer replacement. */
  void CancelSession(uint8_t voice, uint8_t session, uint16_t wave_id);

  /** Snapshot pending urgent voices, newest first. Writes at most 8 ids. */
  unsigned UrgentVoices(uint8_t *dst);
  /** Current source consumption and attack-to-BODY lead for scheduling. */
  double VoiceSourceSamplesPerMs(uint8_t voice) const;
  double VoiceAttackLeadMs(uint8_t voice) const;

  /** Key-up: card releases. Body stream continues until Silence. */
  void NoteOff(uint8_t voice);
  void Silence(uint8_t voice);
  void AllNotesOff();

  /** Apply queued control commands while the UAC callback is stopped.
   *  There must be no other command consumer while this runs. */
  void DrainPendingCommands();

  bool AnyActive() const;

  /** Wave id the BODY thread is streaming for this voice, or 0xFFFF if idle. */
  uint16_t LiveWave(uint8_t voice) const;

  /** Advance attack-join clock / file cursor. Does not pace BODY. */
  void ConsumeOutputSamples(double nframes);

  /** Samples this voice can take now (0 if inactive / enough time / full). */
  unsigned WantBurst(uint8_t voice) const;

  /** Direct UAC path: remaining credit available in 1 ms packets. */
  unsigned WantUacSamples(uint8_t voice) const;

  /** Direct UAC path: newest urgent or hungriest credited voice. */
  uint8_t HungriestUacWant(unsigned frame_samples);

  /** Active voice with the least remaining time that still wants a burst.
   *  0xFF if none. Prefers the card's vq `best` when that voice wants data. */
  uint8_t HungriestWant() const;

  /** Wanting voices, hungriest first (vq `best` first when it wants data).
   *  Writes at most kSampleVoices ids into dst. Returns the count. */
  unsigned WantingVoices(uint8_t *dst) const;

  /** Divide a shared sample budget among wanting voices in proportion to
   *  source consumption. Each grant is bounded by WantBurst(). */
  unsigned AllocateBursts(const uint8_t *voices, unsigned nvoices,
                          unsigned budget, unsigned *grants) const;

  /** Aggregate source demand helper used by diagnostics/tests. */
  unsigned SourceDemandSamples(double interval_ms) const;

  /** Copy up to max_n body samples. Sets sof/session. */
  unsigned FillBurst(uint8_t voice, int16_t *dst, unsigned max_n, bool &sof,
                     uint8_t &session);

  /** Fill one direct UAC packet. Urgent packets repeat SOF until vq confirms
   * that the card accepted BODY for the session. */
  unsigned FillUacFrame(uint8_t voice, int16_t *dst, unsigned max_n,
                        bool &sof, uint8_t &session);

  /** Record one routed UAC frame and return its wrapping wire sequence.
   * Called only by the UAC producer immediately after FillUacFrame. */
  uint16_t RecordUacSubmission(uint8_t voice, uint16_t nsamp);

  /** Undo FillBurst when the bulk OUT did not accept the packet. */
  void AbortBurst(uint8_t voice, uint8_t session, uint16_t wave_id,
                  unsigned nsamp, bool sof);
  /** Compatibility overload for callers that only operate on current data. */
  void AbortBurst(uint8_t voice, unsigned nsamp, bool sof);

  /** Record successful host-to-device completion. */
  void CommitBurst(uint8_t voice, uint8_t session, uint16_t wave_id,
                   unsigned nsamp, bool sof);
  /** Compatibility overload for callers that only operate on current data. */
  void CommitBurst(uint8_t voice, unsigned nsamp, bool sof);

  /** True only while a queued chunk still belongs to the live note. */
  bool BurstIsCurrent(uint8_t voice, uint8_t session,
                      uint16_t wave_id) const;

  /** Authoritative RS-485 `vq` with exact per-voice free-sample counts. */
  void ApplyVoiceStatus(const cardproto::VoiceQuery &status,
                        const uint16_t *unreflected = nullptr);

private:
  enum class CmdKind : uint8_t { On, Silence, Cancel, AllOff };

  struct Cmd {
    CmdKind kind = CmdKind::On;
    uint8_t voice = 0;
    uint16_t wave_id = 0xFFFFu;
    double attack_elapsed_ms = 0.0;
    double source_hz = 0.0;
    uint8_t session = 0u;
    uint32_t epoch = 0u;
  };

  struct Voice {
    bool active = false;
    bool sof_pending = false;
    bool urgent_pending = false;
    uint8_t session = 0;
    uint16_t wave_id = 0;
    double source_hz = 0.0;
    double cursor = 0.0;
    double phase = 0.0;
    double queued = 0.0;
    double attack_elapsed_ms = 0.0;
    unsigned attack_len = 0;
    uint32_t vq_seen = 0;
    unsigned vq_free = 0;
    unsigned vq_card_fill = 0;
    unsigned sent_this_vq = 0;
    bool uac_started = false;
    uint32_t urgent_epoch = 0u;
    bool vq_have = false;
  };

  struct UacLedgerEntry {
    uint32_t absolute = 0u;
    uint16_t nsamp = 0u;
    uint8_t voice = 0xFFu;
  };

  static constexpr unsigned kCmdCap = 32;

  bool Post(const Cmd &c);
  void DrainCmds();
  void ApplyCmd(const Cmd &c);
  int16_t NextBody(Voice &v);
  double IncOf(const Voice &v) const;
  static double Fade0Of(unsigned attack_len);
  static bool BodyDraining(const Voice &v);
  void PullVq();
  double RemainingMs(const Voice &v) const;
  bool WaveInUse(uint16_t wave_id) const;

  std::array<Cmd, kCmdCap> cmds_{};
  std::mutex cmd_post_mutex_;
  std::atomic<uint32_t> cmd_wr_{0};
  std::atomic<uint32_t> cmd_rd_{0};

  std::array<Voice, kSampleVoices> voices_{};
  std::array<std::atomic<unsigned>, kSampleVoices> sent_{};
  std::array<std::atomic<unsigned>, kSampleVoices> committed_{};
  std::array<std::atomic<uint8_t>, kSampleVoices> next_session_{};
  std::array<std::atomic<bool>, kSampleVoices> live_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> live_wave_{};
  std::atomic<uint32_t> vq_seq_{1};
  std::atomic<uint8_t> vq_mask_{0};
  std::atomic<uint8_t> vq_best_{0xFF};
  std::atomic<uint16_t> vq_capacity_{0};
  std::atomic<uint8_t> vq_pending_mask_{0};
  std::array<std::atomic<uint8_t>, kSampleVoices> vq_session_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_fill_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_fill_max_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_free_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_unreflected_{};
  std::atomic<uint16_t> vq_uac_sequence_{0u};
  static constexpr uint32_t kUacLedgerCapacity = 256u;
  std::array<UacLedgerEntry, kUacLedgerCapacity> uac_ledger_{};
  uint32_t uac_published_count_ = 0u;
  uint32_t uac_ack_count_ = 0u;
  uint16_t uac_base_sequence_ = 0u;
  bool uac_sequence_aligned_ = false;
  std::atomic<uint32_t> note_epoch_{0u};
  std::atomic<unsigned> urgent_ready_{0u};

  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<std::atomic<double>, 256> root_hz_{};
  std::array<std::atomic<bool>, 256> oneshot_{};
  std::array<std::atomic<unsigned>, 256> attack_len_{};
};

} // namespace audio
} // namespace cardlink

#endif /* CARDLINK_AUDIO_SAMPLE_DRY_HPP */
