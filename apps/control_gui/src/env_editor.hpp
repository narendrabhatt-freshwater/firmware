#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct App;

/** One linear ramp: end amp (release forced to 0), slope > 0, pitch-track k. */
struct EnvSegment
{
  float end_amp = 1.f; // ignored for last (release) segment
  float slope = 1.f;   // |Δamp| per second
  float k = 0.f;       // rate *= (f/C4)^k, range [-10, 10]
};

/**
 * Host-side envelope program mirroring Channel `en` / `en0`…`enf`.
 * Segments include release as the last entry (count 2..10).
 */
struct EnvProgram
{
  static constexpr int kMinSegs = 2;
  static constexpr int kMaxSegs = 10;
  static constexpr float kC4Hz = 261.625565f;

  std::vector<EnvSegment> segs;

  EnvProgram();

  void ResetPluck();
  void ResetPad();
  void ResetOrgan();
  void ResetSnappy();
  void EnsureValid();

  int PreReleaseCount() const { return static_cast<int>(segs.size()) - 1; }
  EnvSegment &Release() { return segs.back(); }
  const EnvSegment &Release() const { return segs.back(); }

  bool AddSegmentBeforeRelease();
  bool RemoveSegmentBeforeRelease(int index);

  /** Build `en` / `enX` token string (without the en / enX prefix). */
  std::string FormatTokens() const;

  /** Full command: en / en0..enf / en + tokens. voice < 0 → all (`en`). */
  std::string FormatCommand(int voice /* -1 = all */) const;

  /** Sample amp 0..1 over normalized time 0..1 for plotting. */
  void SampleCurve(float *out, int n, float *out_duration_sec = nullptr) const;

  /** Duration of one segment (seconds) given start amp. */
  static float SegDuration(float start_amp, const EnvSegment &seg, bool is_release);

  /** Pitch-rate multiplier at freq_hz for k. */
  static float PitchRate(float freq_hz, float k);
};

/** Draw Tone-tab envelope UI; queues Apply via app.bus when user commits. */
void DrawEnvelopeEditor(App &app);

/** Oscillator shape + filter strip for Tone tab. */
void DrawToneShapeAndFilter(App &app);

/** Richer Effect card panel. */
void DrawEffectPanel(App &app);
