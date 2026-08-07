/**
 * @file channel.hpp
 * @brief Typed Channel Card console API (console protocol specification §2).
 *
 * ChannelClient validates args then Exchange()s via IConsoleTransport.
 * Format* helpers only encode strings (no validation, no CR).
 */

#ifndef PROTOCOL_CHANNEL_HPP
#define PROTOCOL_CHANNEL_HPP

#include "protocol/result.hpp"
#include "protocol/transport.hpp"

#include <cstdint>
#include <string>

namespace protocol {

/** Wire: m 0 = notes (DDS), m 1 = wave (one-shot banks). */
enum class PlayMode : uint8_t { Notes = 0, Wave = 1 };

/** Wire: cpu / cpu N / cpu q [N] / cpu 0. */
enum class CpuProbe : uint8_t { Off = 0, On = 1, Queue = 2 };

class ChannelClient {
public:
  explicit ChannelClient(IConsoleTransport &transport);

  Result Help();                      /**< h */
  Result Exec(const std::string &command); /**< raw line, still Target::Channel */

  /* ---- notes (n0…nf, n) ---- */
  /** Bare n0 — session defaults (bypass on, g 1 0); does not start a tone. */
  Result NoteDefaults();
  /**
   * slot 0…15; hz in [20, 20000) or 0 = off; scale in [0, 1] or omit (-1)
   * for card default 0.125. Fractional hz required for equal temperament.
   */
  Result SetNote(uint8_t slot, double hz, double scale = -1.0);
  Result NoteOff(uint8_t slot); /**< nX 0 */
  Result SetAllNotes(double hz, double scale = -1.0);
  Result AllNotesOff(); /**< n 0 */

  /* ---- playback mode (m) ---- */
  Result GetMode(); /**< m → ok: m 0|1 */
  Result SetMode(PlayMode mode);

  /* ---- wave play (w / w0…w7); upload is wl over CDC, not here ---- */
  Result QueryWaves();
  Result QueryWave(uint8_t slot); /**< slot 0…7 */
  /** rate samples/s in [1, 192000]; pitch mapping uses rate/128. */
  Result PlayWave(uint8_t slot, double rate_hz);
  Result StopWave(uint8_t slot);

  /* ---- shape (global) ---- */
  Result Sine();                      /**< s */
  Result Pulse(double duty);          /**< p; duty [0.1, 0.9] */
  Result Triangle(double asymmetry);  /**< t; [0.1, 0.9], 0.5 = symmetric */

  /* ---- envelope (en / ek) ---- */
  Result ListEnvelopes();
  /** tokens = "end slope[±k] … release" or "0" to clear — see protocol spec */
  Result SetEnvelopeAll(const std::string &tokens);
  Result ClearEnvelopeAll();
  Result GetEnvelope(uint8_t slot);
  Result SetEnvelope(uint8_t slot, const std::string &tokens);
  Result ClearEnvelope(uint8_t slot);
  Result GetEnvK();
  Result SetEnvK(double k); /**< k in [-10, 10] */
  Result GetEnvK(uint8_t slot);
  Result SetEnvK(uint8_t slot, double k);

  /* ---- digital LPF voices 0…7 only (f / fk) ---- */
  Result GetFilters();
  /** hz [20, 20000]; 0 or 20000 = bypass; q in [0.5, 10] or omit (-1). */
  Result SetFilters(double hz, double q = -1.0);
  Result GetFilter(uint8_t slot);
  Result SetFilter(uint8_t slot, double hz, double q = -1.0);
  Result GetFk();
  Result SetFk(double k); /**< k in [0, 10] */
  Result GetFk(uint8_t slot);
  Result SetFk(uint8_t slot, double k);

  /* ---- DAC gain (g) ---- */
  /** ch 1…4; atten_db 0…127. */
  Result SetGain(uint8_t ch, uint8_t atten_db);

  /* ---- cpu load probe (bring-up) ---- */
  /** nvoices 1…16 when On/Queue with N; 0 = firmware default / off path. */
  Result Cpu(CpuProbe kind, uint8_t nvoices = 0);

private:
  IConsoleTransport &tx_;
  Result Send(const std::string &cmd);
};

/* Pure encoders — no range checks, no CR. */
std::string FormatSetNote(uint8_t slot, double hz, double scale = -1.0);
std::string FormatSetAllNotes(double hz, double scale = -1.0);
std::string FormatMode(PlayMode mode);
std::string FormatPlayWave(uint8_t slot, double rate_hz);
std::string FormatStopWave(uint8_t slot);
std::string FormatPulse(double duty);
std::string FormatTriangle(double asymmetry);
std::string FormatSetEnvelope(uint8_t slot, const std::string &tokens);
std::string FormatSetEnvelopeAll(const std::string &tokens);
std::string FormatSetFilter(uint8_t slot, double hz, double q = -1.0);
std::string FormatSetFilters(double hz, double q = -1.0);
std::string FormatSetFk(uint8_t slot, double k);
std::string FormatSetFkAll(double k);
std::string FormatSetEnvK(uint8_t slot, double k);
std::string FormatSetEnvKAll(double k);
std::string FormatGain(uint8_t ch, uint8_t atten_db);
std::string FormatCpu(CpuProbe kind, uint8_t nvoices = 0);

} // namespace protocol

#endif /* PROTOCOL_CHANNEL_HPP */
