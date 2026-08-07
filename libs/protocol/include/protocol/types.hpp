/**
 * @file types.hpp
 * @brief Wire addressing and exchange status (no I/O).
 */

#ifndef PROTOCOL_TYPES_HPP
#define PROTOCOL_TYPES_HPP

#include <cstdint>
#include <string>

namespace protocol {

/**
 * Console address for multi-drop RS485. On a dedicated USB CDC link the
 * prefix is optional; clients still pass Channel/Effect so a shared-bus
 * transport can emit c: / e:.
 */
enum class Target : uint8_t {
  Channel = 0, /**< wire prefix "c:" */
  Effect = 1,  /**< wire prefix "e:" */
  All = 2      /**< wire prefix "*:" (broadcast) */
};

/** Outcome of one request/response (or a local validation failure). */
enum class Status : uint8_t {
  Ok = 0,   /**< body ok / ok:… */
  Err,      /**< body err:… or LocalErr */
  Timeout,  /**< transport: no reply */
  IoError,  /**< transport: write/read failed */
  BadReply, /**< line not ok:/err: */
  BusBusy,  /**< reserved */
};

/** Returns "c:", "e:", or "*:". */
std::string TargetPrefix(Target t);

} // namespace protocol

#endif /* PROTOCOL_TYPES_HPP */
