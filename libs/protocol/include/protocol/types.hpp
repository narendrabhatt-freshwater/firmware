/**
 * @file types.hpp
 * @brief Wire addressing and exchange status codes (no I/O).
 *
 * These types are shared by Format helpers, ParseReplyBody, and typed
 * clients. Transports map Status::Timeout / IoError; cards produce Ok / Err
 * bodies.
 */

#ifndef PROTOCOL_TYPES_HPP
#define PROTOCOL_TYPES_HPP

#include <cstdint>
#include <string>

namespace protocol {

/**
 * @brief Console address for multi-drop RS485 (and optional CDC prefixes).
 *
 * On a dedicated USB CDC link the prefix may be omitted; clients still pass
 * Channel or Effect so a shared-bus transport can emit `c:` / `e:`.
 */
enum class Target : uint8_t {
  Channel = 0, /**< Wire prefix `c:`. */
  Effect = 1,  /**< Wire prefix `e:`. */
  All = 2      /**< Wire prefix `*: ` (broadcast). */
};

/**
 * @brief Outcome of one request/response, or a local validation failure.
 *
 * @note Local validation uses Err via Result::LocalErr and does not transmit.
 */
enum class Status : uint8_t {
  Ok = 0,   /**< Reply body is `ok` or `ok:…`. */
  Err,      /**< Reply body is `err:…`, or Result::LocalErr. */
  Timeout,  /**< Transport: no terminal reply within the deadline. */
  IoError,  /**< Transport: write/read failed. */
  BadReply, /**< A line arrived but was neither `ok`/`ok:` nor `err:`. */
  BusBusy,  /**< Reserved for host bus arbitration; unused by this library. */
};

/**
 * @brief ASCII address prefix for @p t.
 *
 * @param[in] t Address selector.
 * @return `"c:"`, `"e:"`, or `"*:"`.
 */
std::string TargetPrefix(Target t);

} // namespace protocol

#endif /* PROTOCOL_TYPES_HPP */
