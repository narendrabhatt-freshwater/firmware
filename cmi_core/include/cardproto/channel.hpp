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

  /** Raw MIDI-key note-on. Wire `nX on key velocity`. */
  Result NoteOn(uint8_t slot, uint8_t key, uint8_t velocity = 127u);

  /** Authoritative streamed note-on. Wire `nX on key velocity @session`; the tag
   * binds the following USB BODY SOF to this exact gate generation. */
  Result StreamNoteOn(uint8_t slot, uint8_t key, uint8_t velocity,
                      uint8_t session);
  /** Source-compatible streamed note-on using full velocity. */
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
   * @brief Query generations, exact ring credit, and processed UAC sequence.
   * @return Versioned `vq7` status (binary on RS485, readable on USB CDC).
   */
  Result QueryVoiceStatus();

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
 * @param[in] slot Voice 0…15 (encoded as `n0`…`nf`).
 * @param[in] key  Raw MIDI key.
 * @param[in] velocity Raw MIDI velocity in 1..127.
 * @return Command string, e.g. `"n0 on 69 127"`.
 */
std::string FormatNoteOn(uint8_t slot, uint8_t key,
                         uint8_t velocity = 127u);

/** Format a note-off (`nX off`). */
std::string FormatNoteOff(uint8_t slot);

/** Format a session-bound streamed note-on (`nX on key velocity @session`). */
std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t velocity,
                               uint8_t session);
/** Source-compatible formatter using full velocity. */
std::string FormatStreamNoteOn(uint8_t slot, uint8_t key, uint8_t session);

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

/** @} */

struct VoiceQuery {
  uint8_t active_mask = 0;
  uint8_t pending_mask = 0;
  uint8_t best = 0xFF; /**< Hungriest voice, or 0xFF if none. */
  uint16_t capacity = 0;
  uint16_t status_sequence = 0;
  uint16_t uac_sequence = 0; /**< Last routed UAC frame processed by card. */
  std::array<uint8_t, 8> target_session{};
  std::array<uint16_t, 8> target_fill{};
  std::array<uint16_t, 8> free_samples{};
};

/** Parse exact-credit `ok:vq`; accepts an optional `[C]`/`[E]` tag. */
bool ParseVoiceQuery(const char *raw, VoiceQuery &out);

} // namespace cardproto

#endif /* PROTOCOL_CHANNEL_HPP */
