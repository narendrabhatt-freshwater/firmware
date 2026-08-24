/**
 * @file types.hpp
 * @brief RS485 tagged-link types — targets, status, link options.
 *
 * Wire contract: host TX is CR-terminated ASCII. General card replies are
 * tagged terminal lines; Channel `vq` uses a fixed 26-byte binary status
 * frame carrying exact ring credit and USB PACK acknowledgement.
 */

#ifndef CARDLINK_RS485_TYPES_HPP
#define CARDLINK_RS485_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

namespace cardlink {
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
  /** Keep aligned with cardproto::Result buffer sizes (transport bridge). */
  static constexpr std::size_t kErrCodeCapacity = 16;
  static constexpr std::size_t kRawCapacity = 96;

  Status status = Status::Timeout;
  Target from = Target::Channel;
  char err_code[kErrCodeCapacity] = {};
  char raw[kRawCapacity] = {};

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
  /** Optional settle after drain before read (0 = none). */
  uint32_t post_tx_settle_ms = 0;
  /**
   * Optional fixed sleep after a tagged ACK before Send returns (0 = none).
   * Prefer WaitRxIdle for bus-clear; keep this at 0 for MIDI.
   */
  uint32_t post_ack_settle_ms = 0;
  /**
   * On Timeout/BadReply, peek this long for a late tagged ACK before
   * FlushInput + resend (avoids discarding a slow [C]ok then colliding).
   */
  uint32_t late_ack_grace_ms = 120;
  /**
   * After a miss, require this much RX silence before retry TX
   * ("is someone still talking?" on A/B — not a fixed bus delay).
   */
  uint32_t rx_idle_ms = 5;
  uint32_t rx_idle_max_ms = 80;
  /** Extra attempts on Timeout/BadReply only (not on Err). */
  int retries = 2;
  /** If set, require reply tag to match this target (All = either). */
  bool enforce_tag = true;
};

/** Parse a terminal tagged line. raw is the body after `[C]` / `[E]`. */
ExchangeResult ParseTaggedReply(const std::string &line, Target expected);

constexpr std::size_t kVqBinaryFrameLen = 26;

/** Validate and translate a Channel binary vq frame to the normal raw body. */
ExchangeResult ParseVqBinaryReply(const uint8_t *frame, std::size_t len);

} // namespace rs485
} // namespace cardlink

#endif // CARDLINK_RS485_TYPES_HPP
