/**
 * @file result.hpp
 * @brief Exchange result and reply-body parser.
 */

#ifndef PROTOCOL_RESULT_HPP
#define PROTOCOL_RESULT_HPP

#include "protocol/types.hpp"

#include <string>

namespace protocol {

struct Result {
  Status status = Status::Timeout;
  Target from = Target::Channel;
  char err_code[16] = {}; /**< e.g. "range", "syntax" (no "err:" prefix) */
  char raw[96] = {};      /**< full body after optional [C]/[E] strip */

  bool ok() const { return status == Status::Ok; }
  bool got_reply() const {
    return status == Status::Ok || status == Status::Err ||
           status == Status::BadReply;
  }

  /** Local validation failure — do not send on the wire. */
  static Result LocalErr(const char *code, const char *detail = nullptr);
  static Result IoErr(const char *detail);
};

/**
 * Classify one reply line (tag optional).
 * Accepts "[C] ok", "[E]err:range", "ok: m 1", etc.
 */
Result ParseReplyBody(const std::string &line, Target expected);

} // namespace protocol

#endif /* PROTOCOL_RESULT_HPP */
