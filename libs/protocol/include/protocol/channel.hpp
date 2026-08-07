/**
 * @file channel.hpp
 * @brief Typed Channel Card console API (console protocol specification §2).
 *
 * @par ChannelClient
 * Validates arguments, then calls IConsoleTransport::Exchange with
 * Target::Channel. Out-of-range args return Result::LocalErr and do not
 * transmit.
 *
 * @par Format*
 * Pure encoders: build the ASCII token string only. No range checks and no
 * trailing CR — the transport appends `\r`.
 *
 * Wave **upload** (`wl` + binary payload) is out of scope; this API covers
 * mode / play / stop only.
 */

#ifndef PROTOCOL_CHANNEL_HPP
#define PROTOCOL_CHANNEL_HPP

#include "protocol/result.hpp"
#include "protocol/transport.hpp"

#include <cstdint>
#include <string>

namespace protocol {

/**
 * @brief Playback engine select (wire `m`).
 * @note Notes = DDS bank (`n0`…`nf`); Wave = one-shot banks on slots 0…7.
 */
enum class PlayMode : uint8_t {
  Notes = 0, /**< Wire `m 0`. */
  Wave = 1   /**< Wire `m 1`. */
};

/**
 * @brief CPU-load LED probe mode (wire `cpu`).
 * @note Bring-up only; not part of the musical control path.
 */
enum class CpuProbe : uint8_t {
  Off = 0,   /**< Wire `cpu 0`. */
  On = 1,    /**< Wire `cpu` or `cpu N`. */
  Queue = 2  /**< Wire `cpu q` or `cpu q N`. */
};

/**
 * @brief High-level Channel Card client over an injected transport.
 *
 * All Exchange calls use Target::Channel. The transport may still prepend
 * `c:` on a shared RS485 bus.
 */
class ChannelClient {
public:
  /**
   * @brief Construct a client bound to @p transport.
   * @param[in] transport Host pipe (not owned; must outlive this client).
   */
  explicit ChannelClient(IConsoleTransport &transport);

  /**
   * @brief Request the one-line help menu.
   * @return Exchange result for wire `h`.
   */
  Result Help();

  /**
   * @brief Send a raw Channel command string (still Target::Channel).
   *
   * @param[in] command ASCII tokens without target prefix or trailing CR.
   * @return Transport exchange result (no local validation of @p command).
   */
  Result Exec(const std::string &command);

  /* ---- notes (n0…nf, n) ---- */

  /**
   * @brief Apply session defaults only (bypass on, `g 1 0`).
   * @note Does not start a tone. Wire: bare `n0`.
   * @return Exchange result.
   */
  Result NoteDefaults();

  /**
   * @brief Set one note slot frequency (and optional amplitude scale).
   *
   * Prefer fractional Hz — integer rounding detunes equal-temperament
   * octaves (e.g. C4→262 / C5→523 instead of 261.63 / 523.25).
   *
   * @param[in] slot  Voice index in `[0, 15]`.
   * @param[in] hz    Frequency Hz in `[20, 20000)`, or `0` to turn the slot off.
   * @param[in] scale Amplitude scale in `[0, 1]`, or `-1` to omit (card default
   *                  0.125).
   * @return LocalErr on bad args; otherwise wire `nX …` exchange result.
   */
  Result SetNote(uint8_t slot, double hz, double scale = -1.0);

  /**
   * @brief Turn one note slot off.
   * @param[in] slot Voice index in `[0, 15]`.
   * @return Equivalent to SetNote(@p slot, 0). Wire `nX 0`.
   */
  Result NoteOff(uint8_t slot);

  /**
   * @brief Set all 16 note slots to the same Hz / scale.
   * @param[in] hz    Frequency Hz in `[20, 20000)`, or `0` for silence.
   * @param[in] scale Amplitude scale in `[0, 1]`, or `-1` to omit.
   * @return LocalErr on bad args; otherwise wire `n …` exchange result.
   */
  Result SetAllNotes(double hz, double scale = -1.0);

  /**
   * @brief Silence all 16 note slots.
   * @return Exchange result for wire `n 0`.
   */
  Result AllNotesOff();

  /* ---- playback mode (m) ---- */

  /**
   * @brief Query the current playback mode.
   * @return Exchange result; success body typically `ok: m 0` or `ok: m 1`.
   */
  Result GetMode();

  /**
   * @brief Select notes or wave playback engine.
   * @param[in] mode Notes (`m 0`) or Wave (`m 1`).
   * @return Exchange result for wire `m 0` / `m 1`.
   */
  Result SetMode(PlayMode mode);

  /* ---- wave play (w / w0…w7); upload is wl over CDC, not here ---- */

  /**
   * @brief Query all wave slot statuses.
   * @return Exchange result for wire `w`.
   */
  Result QueryWaves();

  /**
   * @brief Query one wave slot.
   * @param[in] slot Wave bank index in `[0, 7]`.
   * @return LocalErr on bad slot; otherwise wire `wX` exchange result.
   */
  Result QueryWave(uint8_t slot);

  /**
   * @brief Start one-shot playback of a loaded wave bank.
   *
   * Pitch mapping uses @p rate_hz / 128. Requires PlayMode::Wave and a prior
   * `wl` upload into that slot (upload is not performed by this library).
   *
   * @param[in] slot     Wave bank index in `[0, 7]`.
   * @param[in] rate_hz  Playback rate in samples/s, range `[1, 192000]`.
   * @return LocalErr on bad args; otherwise wire `wX <rate>` exchange result.
   */
  Result PlayWave(uint8_t slot, double rate_hz);

  /**
   * @brief Stop one wave slot.
   * @param[in] slot Wave bank index in `[0, 7]`.
   * @return LocalErr on bad slot; otherwise wire `wX 0` exchange result.
   */
  Result StopWave(uint8_t slot);

  /* ---- shape (global) ---- */

  /**
   * @brief Select global sine shape for the note bank.
   * @return Exchange result for wire `s`.
   */
  Result Sine();

  /**
   * @brief Select global pulse shape.
   * @param[in] duty Pulse duty cycle in `[0.1, 0.9]`.
   * @return LocalErr on bad duty; otherwise wire `p …` exchange result.
   */
  Result Pulse(double duty);

  /**
   * @brief Select global triangle shape.
   * @param[in] asymmetry Triangle asymmetry in `[0.1, 0.9]` (`0.5` = symmetric).
   * @return LocalErr on bad value; otherwise wire `t …` exchange result.
   */
  Result Triangle(double asymmetry);

  /* ---- envelope (en / ek) ---- */

  /**
   * @brief List programmed envelopes.
   * @return Exchange result for wire `en`.
   */
  Result ListEnvelopes();

  /**
   * @brief Program the amplitude envelope on all 16 voices.
   *
   * @param[in] tokens Segment list (`end slope[±k] … release`) or `"0"` to
   *                   clear; exact grammar is defined by the protocol spec.
   * @return Exchange result for wire `en <tokens>`.
   */
  Result SetEnvelopeAll(const std::string &tokens);

  /**
   * @brief Clear envelopes on all voices (unprogrammed bypass).
   * @return Exchange result for wire `en 0`.
   */
  Result ClearEnvelopeAll();

  /**
   * @brief Query one voice envelope.
   * @param[in] slot Voice index in `[0, 15]`.
   * @return LocalErr on bad slot; otherwise wire `enX` exchange result.
   */
  Result GetEnvelope(uint8_t slot);

  /**
   * @brief Program one voice envelope.
   * @param[in] slot   Voice index in `[0, 15]`.
   * @param[in] tokens Segment list or `"0"` to clear.
   * @return LocalErr on bad slot; otherwise wire `enX …` exchange result.
   */
  Result SetEnvelope(uint8_t slot, const std::string &tokens);

  /**
   * @brief Clear one voice envelope.
   * @param[in] slot Voice index in `[0, 15]`.
   * @return LocalErr on bad slot; otherwise wire `enX 0` exchange result.
   */
  Result ClearEnvelope(uint8_t slot);

  /**
   * @brief Query global envelope pitch-track coefficient.
   * @return Exchange result for wire `ek`.
   */
  Result GetEnvK();

  /**
   * @brief Set global envelope pitch-track coefficient.
   * @param[in] k Coefficient in `[-10, 10]`.
   * @return LocalErr on bad @p k; otherwise wire `ek …` exchange result.
   */
  Result SetEnvK(double k);

  /**
   * @brief Query one voice envelope pitch-track coefficient.
   * @param[in] slot Voice index in `[0, 15]`.
   * @return LocalErr on bad slot; otherwise wire `ekX` exchange result.
   */
  Result GetEnvK(uint8_t slot);

  /**
   * @brief Set one voice envelope pitch-track coefficient.
   * @param[in] slot Voice index in `[0, 15]`.
   * @param[in] k    Coefficient in `[-10, 10]`.
   * @return LocalErr on bad args; otherwise wire `ekX …` exchange result.
   */
  Result SetEnvK(uint8_t slot, double k);

  /* ---- digital LPF voices 0…7 only (f / fk) ---- */

  /**
   * @brief Query filters on voices 0…7.
   * @return Exchange result for wire `f`.
   */
  Result GetFilters();

  /**
   * @brief Set cutoff (and optional Q) on voices 0…7.
   *
   * @param[in] hz Cutoff Hz in `[20, 20000]`; `0` or `20000` = bypass.
   * @param[in] q  Resonance in `[0.5, 10]`, or `-1` to omit.
   * @return LocalErr on bad args; otherwise wire `f …` exchange result.
   */
  Result SetFilters(double hz, double q = -1.0);

  /**
   * @brief Query one filter (voices 0…7 only).
   * @param[in] slot Voice index in `[0, 7]`.
   * @return LocalErr on bad slot; otherwise wire `fX` exchange result.
   */
  Result GetFilter(uint8_t slot);

  /**
   * @brief Set one voice filter.
   * @param[in] slot Voice index in `[0, 7]`.
   * @param[in] hz   Cutoff Hz in `[20, 20000]`; `0` or `20000` = bypass.
   * @param[in] q    Resonance in `[0.5, 10]`, or `-1` to omit.
   * @return LocalErr on bad args; otherwise wire `fX …` exchange result.
   */
  Result SetFilter(uint8_t slot, double hz, double q = -1.0);

  /**
   * @brief Query global filter pitch-track coefficient.
   * @return Exchange result for wire `fk`.
   */
  Result GetFk();

  /**
   * @brief Set global filter pitch-track coefficient.
   * @param[in] k Coefficient in `[0, 10]`.
   * @return LocalErr on bad @p k; otherwise wire `fk …` exchange result.
   */
  Result SetFk(double k);

  /**
   * @brief Query one voice filter pitch-track coefficient.
   * @param[in] slot Voice index in `[0, 7]`.
   * @return LocalErr on bad slot; otherwise wire `fkX` exchange result.
   */
  Result GetFk(uint8_t slot);

  /**
   * @brief Set one voice filter pitch-track coefficient.
   * @param[in] slot Voice index in `[0, 7]`.
   * @param[in] k    Coefficient in `[0, 10]`.
   * @return LocalErr on bad args; otherwise wire `fkX …` exchange result.
   */
  Result SetFk(uint8_t slot, double k);

  /* ---- DAC gain (g) ---- */

  /**
   * @brief Set CS4304 attenuation on one DAC channel.
   * @param[in] ch       Hardware channel in `[1, 4]`.
   * @param[in] atten_db Attenuation in dB, range `[0, 127]`.
   * @return LocalErr on bad args; otherwise wire `g …` exchange result.
   */
  Result SetGain(uint8_t ch, uint8_t atten_db);

  /* ---- cpu load probe (bring-up) ---- */

  /**
   * @brief Drive the CPU-load LED probe.
   *
   * @param[in] kind    Off / On / Queue.
   * @param[in] nvoices Voice count in `[1, 16]` when On/Queue with N; `0`
   *                    selects the firmware default / off path for that kind.
   * @return LocalErr on bad @p nvoices; otherwise wire `cpu …` exchange result.
   */
  Result Cpu(CpuProbe kind, uint8_t nvoices = 0);

private:
  IConsoleTransport &tx_;
  Result Send(const std::string &cmd);
};

/**
 * @name Format helpers (Channel)
 * @brief Encode wire tokens only — no validation, no trailing CR.
 * @{
 */

/**
 * @param[in] slot  Voice 0…15 (encoded as `n0`…`nf`).
 * @param[in] hz    Frequency or `0` for off.
 * @param[in] scale Scale, or `< 0` to omit from the wire string.
 * @return Command string, e.g. `"n0 261.625565"`.
 */
std::string FormatSetNote(uint8_t slot, double hz, double scale = -1.0);

/**
 * @param[in] hz    Frequency or `0` for silence-all.
 * @param[in] scale Scale, or `< 0` to omit.
 * @return Command string for wire `n …`.
 */
std::string FormatSetAllNotes(double hz, double scale = -1.0);

/**
 * @param[in] mode Notes or Wave.
 * @return `"m 0"` or `"m 1"`.
 */
std::string FormatMode(PlayMode mode);

/**
 * @param[in] slot    Wave bank 0…7.
 * @param[in] rate_hz Samples/s.
 * @return Command string for wire `wX <rate>`.
 */
std::string FormatPlayWave(uint8_t slot, double rate_hz);

/**
 * @param[in] slot Wave bank 0…7.
 * @return Command string for wire `wX 0`.
 */
std::string FormatStopWave(uint8_t slot);

/**
 * @param[in] duty Pulse duty.
 * @return Command string for wire `p …`.
 */
std::string FormatPulse(double duty);

/**
 * @param[in] asymmetry Triangle asymmetry.
 * @return Command string for wire `t …`.
 */
std::string FormatTriangle(double asymmetry);

/**
 * @param[in] slot   Voice 0…15.
 * @param[in] tokens Envelope token list or `"0"`.
 * @return Command string for wire `enX …`.
 */
std::string FormatSetEnvelope(uint8_t slot, const std::string &tokens);

/**
 * @param[in] tokens Envelope token list or `"0"`.
 * @return Command string for wire `en …`.
 */
std::string FormatSetEnvelopeAll(const std::string &tokens);

/**
 * @param[in] slot Voice 0…7.
 * @param[in] hz   Cutoff Hz.
 * @param[in] q    Q, or `< 0` to omit.
 * @return Command string for wire `fX …`.
 */
std::string FormatSetFilter(uint8_t slot, double hz, double q = -1.0);

/**
 * @param[in] hz Cutoff Hz.
 * @param[in] q  Q, or `< 0` to omit.
 * @return Command string for wire `f …`.
 */
std::string FormatSetFilters(double hz, double q = -1.0);

/**
 * @param[in] slot Voice 0…7.
 * @param[in] k    Filter pitch-track coefficient.
 * @return Command string for wire `fkX …`.
 */
std::string FormatSetFk(uint8_t slot, double k);

/**
 * @param[in] k Filter pitch-track coefficient.
 * @return Command string for wire `fk …`.
 */
std::string FormatSetFkAll(double k);

/**
 * @param[in] slot Voice 0…15.
 * @param[in] k    Envelope pitch-track coefficient.
 * @return Command string for wire `ekX …`.
 */
std::string FormatSetEnvK(uint8_t slot, double k);

/**
 * @param[in] k Envelope pitch-track coefficient.
 * @return Command string for wire `ek …`.
 */
std::string FormatSetEnvKAll(double k);

/**
 * @param[in] ch       DAC channel 1…4.
 * @param[in] atten_db Attenuation dB 0…127.
 * @return Command string for wire `g …`.
 */
std::string FormatGain(uint8_t ch, uint8_t atten_db);

/**
 * @param[in] kind    Probe mode.
 * @param[in] nvoices Optional voice count (`0` = omit N / off path).
 * @return Command string for wire `cpu …`.
 */
std::string FormatCpu(CpuProbe kind, uint8_t nvoices = 0);

/** @} */

} // namespace protocol

#endif /* PROTOCOL_CHANNEL_HPP */
