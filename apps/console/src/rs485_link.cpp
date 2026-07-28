#include "rs485_link.hpp"

#include <algorithm>
#include <cctype>

namespace rs485 {

std::string TargetPrefix(Target t) {
  switch (t) {
  case Target::Channel:
    return "c:";
  case Target::Effect:
    return "e:";
  case Target::All:
    return "*:";
  }
  return "*:";
}

std::string TargetName(Target t) {
  switch (t) {
  case Target::Channel:
    return "channel";
  case Target::Effect:
    return "effect";
  case Target::All:
    return "all";
  }
  return "all";
}

namespace {
std::string ToLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}
} // namespace

bool ParseTarget(const std::string &s_in, Target &out) {
  std::string s = ToLower(s_in);
  if (s == "channel" || s == "c") {
    out = Target::Channel;
    return true;
  }
  if (s == "effect" || s == "e") {
    out = Target::Effect;
    return true;
  }
  if (s == "all" || s == "*") {
    out = Target::All;
    return true;
  }
  return false;
}

RS485Link::RS485Link(SerialPort &port, LinkOptions opts)
    : port_(port), opts_(opts) {}

ExchangeResult RS485Link::Send(Target target, const std::string &command_in) {
  /* Console_Poll() lowercases every typed character before dispatch (see
   * apps/channel_card/Core/Src/main.c) — match that here so an explicit
   * "E:status" from a user types the same as "e:status" on the wire. */
  std::string command = ToLower(command_in);

  /* RS485_IsForMe() treats any "X:" (line[1] == ':') as an explicit
   * address, valid or not — pass it through unprefixed rather than
   * double-prefixing (e.g. typing "e:status" while the REPL's default
   * target is "channel" must still reach only the Effect Card). */
  bool explicit_prefix = command.size() >= 2 && command[1] == ':';
  std::string line = explicit_prefix ? command : (TargetPrefix(target) + command);
  std::string wire = line + "\r\n";

  ExchangeResult result;
  for (int attempt = 0; attempt <= opts_.retries; attempt++) {
    if (port_.ManualRtsControl())
      port_.SetRts(true);
    bool ok = port_.Write(reinterpret_cast<const uint8_t *>(wire.data()), wire.size());
    if (port_.ManualRtsControl())
      port_.SetRts(false);
    if (!ok) {
      return result; /* hard I/O error — got_reply stays false */
    }

    std::string reply = ReadReply();
    if (!reply.empty()) {
      result.got_reply = true;
      result.reply = std::move(reply);
      return result;
    }
    /* Nothing came back within the window — could be a busy bus (someone
     * else mid-transmission) rather than "no card out there"; retry. */
  }
  return result;
}

std::string RS485Link::ReadReply() {
  uint8_t buf[512];
  std::string out;

  size_t n = port_.ReadTimeout(buf, sizeof(buf), opts_.reply_timeout_ms);
  if (n == 0)
    return out; /* nothing arrived in the initial window */
  out.append(reinterpret_cast<char *>(buf), n);

  /* Keep draining while bytes keep arriving — multi-line replies like
   * `status` span several RS485_Reply() calls in quick succession. */
  while (true) {
    size_t more = port_.ReadTimeout(buf, sizeof(buf), opts_.idle_gap_ms);
    if (more == 0)
      break;
    out.append(reinterpret_cast<char *>(buf), more);
  }
  return out;
}

} // namespace rs485
