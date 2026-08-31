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
 */

#ifndef PROTOCOL_CHANNEL_HPP
#define PROTOCOL_CHANNEL_HPP

#include "cardproto/result.hpp"
#include "cardproto/transport.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace cardproto {

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

  /* ---- notes (n0…n7, n) ---- */

  /**
   * @brief Apply session defaults only (bypass on, `g 1 0`).
   * @note Does not start a tone. Wire: bare `n0`.
   * @return Exchange result.
   */
  Result NoteDefaults();

  /** Raw MIDI-key note-on. Wire `nX on key`. */
  Result NoteOn(uint8_t slot, uint8_t key);

  /** Authoritative streamed note-on. Wire `nX Hz scale @session`; the tag
   * binds the following USB BODY SOF to this exact gate generation. */
  Result StreamNoteOn(uint8_t slot, uint8_t key, uint8_t session);

  /**
   * @brief Turn one note slot off.
   * @param[in] slot Voice index in `[0, 7]`.
   * @return Note-off result. Wire `nX off`.
   */
  Result NoteOff(uint8_t slot);

  /**
   * @brief Silence all 8 note slots.
   * @return Exchange result for wire `n off`.
   */
  Result AllNotesOff();

  /**
   * @brief Query active mask, hungriest voice, exact ring credit and PACK ACK.
   * @return `ok:vq <mask> <best> free0..free7 last_pack_sequence`.
   */
  Result QueryVoiceStatus();

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

  /** Select the phase-derived saw oscillator. Wire `saw`. */
  Result Saw();

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
std::string FormatNoteOn(uint8_t slot, uint8_t key);

/** Format a note-off (`nX off`). */
std::string FormatNoteOff(uint8_t slot);

/** Format a session-bound streamed note-on (`nX Hz scale @session`). */
std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t session);

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

struct VoiceQuery {
  uint8_t mask = 0;
  uint8_t best = 0xFF; /**< Hungriest voice, or 0xFF if none. */
  std::array<uint16_t, 8> free_samples{};
  uint16_t last_pack_sequence = 0xFFFFu;
};

/** Parse exact-credit `ok:vq`; accepts an optional `[C]`/`[E]` tag. */
bool ParseVoiceQuery(const char *raw, VoiceQuery &out);

} // namespace cardproto

#endif /* PROTOCOL_CHANNEL_HPP */
