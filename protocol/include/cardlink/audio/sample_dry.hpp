/**
 * @file sample_dry.hpp
 * @brief Host body feeder: unpitched int16 → UAC2 BODY packs.
 *
 * Card owns pitch / env / filter. The host reserves one session for nX and
 * USB SOF, launches the urgent newest-first BODY job, then queues authoritative
 * nX without waiting for USB or `vq`. Every later
 * RS-485 `vq` grants
 * at most one steady-state refill to each voice (at most kBodyBurstMax and at most the
 * safe free-space credit). Each vq reconciles predicted occupancy to the
 * the card's exact occupancy plus per-voice samples in concurrent USB OUT.
 * One PACK divides its payload by source consumption rate so a fast
 * voice cannot consume its share before the next status.
 *
 * One asynchronous packed USB OUT submission is made per granted status.
 *
 * `queued` estimates card FIFO occupancy (attack does not consume body).
 *
 * UI note changes reach the BODY thread through an SPSC command queue.
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
constexpr unsigned kRingSamples = 12240;
/** Leave room for interpolator taps. */
constexpr unsigned kRingHeadroom = 32;
/** Coalesce refill credit to amortize eight per-voice BODY metas. */
constexpr unsigned kMinBurst = 512;
constexpr unsigned kStreamSessionMod = 255;
/** One BODY meta remains bounded; the larger ring absorbs poll/host jitter. */
constexpr unsigned kBodyBurstMax = 4096;
/** Covers one 5 ms vq period plus observed USB/worker scheduling spikes. */
constexpr unsigned kUrgentPrefillMs = 25;
/** Upper bound for one fair shared startup horizon. */
constexpr double kUrgentQuantumMaxMs = 5.0;
constexpr unsigned kUrgentQueueVoices = kSampleVoices;
constexpr double kDefaultBodyRootHz = 261.625565;

struct UrgentBurst {
  uint8_t voice = 0u;
  uint8_t session = 0u;
  uint16_t wave_id = 0xFFFFu;
  unsigned nsamp = 0u;
  bool sof = false;
};

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

  /** Queue a new urgent note session. Returns its synchronous session id. */
  uint8_t NoteOn(uint8_t voice, uint16_t wave_id, double freq_hz,
                 double attack_elapsed_ms = 0.0);

  /** Allocate the identity that must be carried by both nX and USB SOF. */
  uint8_t ReserveSession(uint8_t voice);

  /** Queue a note using a session already bound into authoritative nX. */
  uint8_t NoteOnSession(uint8_t voice, uint16_t wave_id, double freq_hz,
                        uint8_t session, double attack_elapsed_ms = 0.0);

  /** Cancel a pre-authority session without touching a newer replacement. */
  void CancelSession(uint8_t voice, uint8_t session, uint16_t wave_id);

  /** Configure the conservative old-tail reservation used by urgent prefill. */
  void SetCrashReleaseMs(uint8_t release_ms);

  /** True while a note command or newest-first urgent prefill is pending. */
  bool UrgentPending() const;

  /** Pop the newest live note and render its vq-independent prefill.
   * Each call renders at most one urgent quantum. */
  bool FillUrgentBurst(int16_t *dst, unsigned max_n, UrgentBurst &info);

  /** Snapshot pending urgent voices, newest first. Writes at most 8 ids. */
  unsigned UrgentVoices(uint8_t *dst);
  /** True while any pending urgent voice still needs its first SOF. */
  bool UrgentSofPending() const;
  /** Reconcile a fresh vq without truncating the ordered startup runway. */
  void EndUrgentPrefill(uint8_t active_mask);

  /** Render one urgent quantum for a specific pending voice. */
  bool FillUrgentBurstForVoice(uint8_t voice, int16_t *dst, unsigned max_n,
                               double quantum_ms, UrgentBurst &info);

  /** Current source consumption and attack-to-BODY lead for scheduling. */
  double VoiceSourceSamplesPerMs(uint8_t voice) const;
  double VoiceAttackLeadMs(uint8_t voice) const;

  /** Restore a canceled, not-yet-rendered urgent SOF BODY job. */
  void AbortUrgentBurst(uint8_t voice, uint8_t session, uint16_t wave_id,
                        unsigned nsamp, bool sof);

  /** Key-up: card releases. Body stream continues until Silence. */
  void NoteOff(uint8_t voice);
  void Silence(uint8_t voice);
  void AllNotesOff();

  bool AnyActive() const;

  /** Wave id the BODY thread is streaming for this voice, or 0xFFFF if idle. */
  uint16_t LiveWave(uint8_t voice) const;

  /** Advance attack-join clock / file cursor. Does not pace BODY. */
  void ConsumeOutputSamples(double nframes);

  /** Samples this voice can take now (0 if inactive / enough time / full). */
  unsigned WantBurst(uint8_t voice) const;

  /** Active voice with the least remaining time that still wants a burst.
   *  0xFF if none. Prefers the card's vq `best` when that voice wants data. */
  uint8_t HungriestWant() const;

  /** Wanting voices, hungriest first (vq `best` first when it wants data).
   *  Writes at most kSampleVoices ids into dst. Returns the count. */
  unsigned WantingVoices(uint8_t *dst) const;

  /** Divide a PACK sample budget among wanting voices in proportion to
   *  source consumption. Each grant is bounded by WantBurst(). */
  unsigned AllocateBursts(const uint8_t *voices, unsigned nvoices,
                          unsigned budget, unsigned *grants) const;

  /** Aggregate source demand helper used by diagnostics/tests. */
  unsigned SourceDemandSamples(double interval_ms) const;

  /** Copy up to max_n body samples. Sets sof/session. */
  unsigned FillBurst(uint8_t voice, int16_t *dst, unsigned max_n, bool &sof,
                     uint8_t &session);

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

  /** Legacy observation helper; note-on never waits for this completion. */
  bool WaitPrefill(uint8_t voice, unsigned timeout_ms);

  /** Authoritative RS-485 `vq` with exact per-voice free-sample counts. */
  void ApplyVoiceStatus(uint8_t mask, uint8_t best,
                        const uint16_t *free_samples,
                        const uint16_t *unreflected = nullptr);

  void SetPitchHz(uint8_t voice, double freq_hz);

  unsigned QueuedSamples(uint8_t voice) const;

private:
  enum class CmdKind : uint8_t { On, Pitch, Silence, Cancel, AllOff };

  struct Cmd {
    CmdKind kind = CmdKind::On;
    uint8_t voice = 0;
    uint16_t wave_id = 0xFFFFu;
    double freq_hz = 0.0;
    double attack_elapsed_ms = 0.0;
    uint8_t session = 0u;
    uint32_t epoch = 0u;
  };

  struct Voice {
    bool active = false;
    bool sof_pending = false;
    bool urgent_pending = false;
    uint8_t session = 0;
    uint16_t wave_id = 0;
    double freq_hz = 0.0;
    double cursor = 0.0;
    double phase = 0.0;
    double queued = 0.0;
    double attack_elapsed_ms = 0.0;
    unsigned attack_len = 0;
    uint32_t vq_seen = 0;
    unsigned vq_free = 0;
    unsigned vq_card_fill = 0;
    unsigned sent_this_vq = 0;
    unsigned urgent_budget = 0u;
    uint32_t urgent_epoch = 0u;
    bool vq_have = false;
  };

  static constexpr unsigned kCmdCap = 32;

  void Post(const Cmd &c);
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
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_fill_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_fill_max_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_free_{};
  std::array<std::atomic<uint16_t>, kSampleVoices> vq_unreflected_{};
  std::atomic<uint32_t> note_epoch_{0u};
  std::atomic<unsigned> urgent_ready_{0u};
  std::atomic<uint8_t> crash_release_ms_{3u};

  std::array<std::vector<int16_t>, 256> bodies_{};
  std::array<std::atomic<double>, 256> root_hz_{};
  std::array<std::atomic<bool>, 256> oneshot_{};
  std::array<std::atomic<unsigned>, 256> attack_len_{};
};

} // namespace audio
} // namespace cardlink

#endif /* CARDLINK_AUDIO_SAMPLE_DRY_HPP */
