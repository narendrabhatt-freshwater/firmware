/**
 * @file result.hpp
 * @brief Exchange result and reply-body classifier.
 */

#ifndef PROTOCOL_RESULT_HPP
#define PROTOCOL_RESULT_HPP

#include "protocol/types.hpp"

#include <cstddef>
#include <string>

namespace protocol {

/**
 * @brief Outcome of one console exchange (or a rejected local call).
 *
 * On success, @ref status is Status::Ok. Card rejects set Status::Err and
 * fill @ref err_code (without the `err:` prefix). Transport failures use
 * Timeout / IoError. Unparseable lines use BadReply.
 *
 * Fixed buffers avoid heap use on the host path; sizes are named so call
 * sites and bridges stay aligned if they grow.
 */
struct Result {
  /** Bytes for @ref err_code including the terminating NUL. */
  static constexpr std::size_t kErrCodeCapacity = 16;
  /** Bytes for @ref raw including the terminating NUL. */
  static constexpr std::size_t kRawCapacity = 96;

  Status status = Status::Timeout; /**< Classified outcome. */
  Target from = Target::Channel;   /**< Card that replied, when tagged. */
  char err_code[kErrCodeCapacity] = {}; /**< e.g. `"range"`, `"syntax"`. */
  char raw[kRawCapacity] = {}; /**< Full body after optional `[C]` / `[E]` strip. */

  /**
   * @brief True when the card (or local path) reported success.
   * @return Whether @ref status equals Status::Ok.
   */
  bool ok() const { return status == Status::Ok; }

  /**
   * @brief True when a reply line was classified (success, err, or bad).
   * @return Whether a terminal body was seen (excludes Timeout / IoError).
   */
  bool got_reply() const {
    return status == Status::Ok || status == Status::Err ||
           status == Status::BadReply;
  }

  /**
   * @brief Build a local validation failure (nothing sent on the wire).
   *
   * @param[in] code   Short token stored in @ref err_code (e.g. `"range"`).
   * @param[in] detail Optional text copied into @ref raw; may be nullptr.
   * @return Result with Status::Err.
   */
  static Result LocalErr(const char *code, const char *detail = nullptr);

  /**
   * @brief Build a transport I/O failure.
   *
   * @param[in] detail Optional text copied into @ref raw; may be nullptr.
   * @return Result with Status::IoError.
   */
  static Result IoErr(const char *detail);
};

/**
 * @brief Classify one reply line into a @ref Result.
 *
 * Accepts tagged RS485 forms (`[C] ok`, `[E]err:range`) and bare CDC bodies
 * (`ok: m 1`). Trailing CR/LF/spaces are ignored.
 *
 * @param[in] line     One reply line (tag optional).
 * @param[in] expected Address used for the request (fills @ref Result::from
 *                     when the line has no `[C]`/`[E]` tag).
 * @return Classified result. Status is Ok, Err, or BadReply for a present line.
 */
Result ParseReplyBody(const std::string &line, Target expected);

} // namespace protocol

#endif /* PROTOCOL_RESULT_HPP */
