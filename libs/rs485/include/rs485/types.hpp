/**
 * @file types.hpp
 * @brief Shared RS485 console types — targets, exchange status, link options.
 *
 * Wire contract: host TX is CR-terminated ASCII; card replies are one
 * tagged terminal line ending in CRLF (`[C]ok\r\n` / `[E]err:range\r\n`).
 * See docs/rs485_console_architecture.md.
 */

#ifndef RS485_TYPES_HPP
#define RS485_TYPES_HPP

#include <cstdint>
#include <cstring>
#include <string>

namespace rs485 {

enum class Target : uint8_t { Channel = 0, Effect = 1, All = 2 };

/** "c:" / "e:" / "*:" — the wire prefix for each target. */
std::string TargetPrefix(Target t);

/** Human-friendly name ("channel"/"effect"/"all"). */
std::string TargetName(Target t);

/** Parses "channel"/"c"/"effect"/"e"/"all"/"*" (case-insensitive). */
bool ParseTarget(const std::string &s, Target &out);

enum class Status : uint8_t {
  Ok = 0,   /**< tagged reply body is ok / ok:... */
  Err,      /**< tagged reply body is err:... (card rejected) */
  Timeout,  /**< no terminal tagged reply within window (after retries) */
  IoError,  /**< port write/read/drain failed */
  BadReply, /**< tagged line present but unparsable / wrong card tag */
  BusBusy,  /**< reserved */
};

struct ExchangeResult {
  Status status = Status::Timeout;
  Target from = Target::Channel;
  char err_code[16] = {};
  char raw[96] = {};

  bool ok() const { return status == Status::Ok; }
  bool got_reply() const {
    return status == Status::Ok || status == Status::Err ||
           status == Status::BadReply;
  }
};

struct LinkOptions {
  uint32_t reply_timeout_ms = 500;
  /** After first tagged byte, stop when idle this long (multi-line help). */
  uint32_t idle_gap_ms = 80;
  /** Extra attempts on Timeout/BadReply only (not on Err). */
  int retries = 2;
  /** If set, require reply tag to match this target (All = either). */
  bool enforce_tag = true;
};

/** Parse a terminal tagged line into ExchangeResult (status/from/err_code/raw). */
ExchangeResult ParseTaggedReply(const std::string &line, Target expected);

} // namespace rs485

#endif // RS485_TYPES_HPP
