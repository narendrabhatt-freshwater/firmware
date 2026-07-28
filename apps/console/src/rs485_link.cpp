#include "rs485_link.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

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

void SleepMs(uint32_t ms) {
  if (ms == 0)
    return;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool IsConsoleTextByte(unsigned char c) {
  return c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f);
}

/** Card replies always start with RS485_TAG ("[C] " / "[E] "). Effect's
 * keystroke echo of our own "c:help\\r\\n" has no tag — keep everything
 * from the first tag onward so echo/noise before it is ignored. */
std::string ExtractTaggedReply(const std::string &raw) {
  size_t c = raw.find("[C]");
  size_t e = raw.find("[E]");
  size_t at = std::string::npos;
  if (c != std::string::npos)
    at = c;
  if (e != std::string::npos && (at == std::string::npos || e < at))
    at = e;
  if (at == std::string::npos)
    return {};

  std::string out = raw.substr(at);
  /* Trailing framing junk after a clean CRLF-terminated reply is common
   * on this bus; trim so PrintReply doesn't treat a good [C]/[E] line as
   * garbage. */
  while (!out.empty() &&
         !IsConsoleTextByte(static_cast<unsigned char>(out.back()))) {
    out.pop_back();
  }
  return out;
}

/** After each TX byte, wait until the RX line goes quiet. The Effect Card
 * echoes every keystroke; if we send the next byte while that echo is
 * still on the wire we collide (PC DE isn't on the MCU RS485_CTL net, so
 * firmware WaitBusFree can't see us). screen avoids this by typing slowly. */
void DrainEcho(SerialPort &port, uint32_t overall_ms) {
  using clock = std::chrono::steady_clock;
  const auto deadline =
      clock::now() + std::chrono::milliseconds(overall_ms);
  uint8_t trash[64];

  /* First wait briefly for at least one echoed byte (or timeout). */
  while (clock::now() < deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - clock::now())
                    .count();
    if (left <= 0)
      return;
    uint32_t slice = static_cast<uint32_t>(std::min<int64_t>(left, 5));
    if (port.ReadTimeout(trash, sizeof(trash), slice) > 0)
      break;
  }

  /* Then keep reading until a short quiet gap (echo burst finished). */
  while (clock::now() < deadline) {
    if (port.ReadTimeout(trash, sizeof(trash), 3) == 0)
      return;
  }
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
  /* Terminate with a single CR — matching what screen/most terminals send
   * on Enter. Firmware Console_Poll() treats BOTH '\\r' and '\\n' as end-
   * of-line and runs the command; sending "\\r\\n" therefore executes twice
   * (second time on an empty buffer). The first reply (especially a long
   * `help` menu) is still on the wire when the LF arrives, so we collide
   * with ourselves and often see "no reply". */
  std::string wire = line + "\r";

  ExchangeResult result;
  for (int attempt = 0; attempt <= opts_.retries; attempt++) {
    port_.FlushInput();

    const auto *bytes = reinterpret_cast<const uint8_t *>(wire.data());
    bool ok = true;
    for (size_t i = 0; i < wire.size(); i++) {
      if (port_.ManualRtsControl())
        port_.SetRts(true);
      ok = port_.Write(bytes + i, 1);
      if (port_.ManualRtsControl())
        port_.SetRts(false);
      if (!ok)
        break;

      SleepMs(1);
      const bool last_byte = (i + 1 == wire.size());
      if (!last_byte) {
        /* Wait out Effect's echo of this keystroke before the next TX
         * byte. Do NOT drain after the final CR — Channel's reply starts
         * ~3 ms later and DrainEcho would eat it. */
        DrainEcho(port_, opts_.inter_byte_ms);
      }
    }
    if (!ok) {
      return result;
    }

    /* Enter: Effect echoes "\r\n"; Channel holds ~3 ms then replies.
     * Do not TX a trailing LF — that would re-trigger Console_Poll while
     * the reply to CR is still being transmitted. */
    SleepMs(5);

    std::string reply = ReadReply();
    if (!reply.empty()) {
      result.got_reply = true;
      result.reply = std::move(reply);
      return result;
    }
  }
  return result;
}

std::string RS485Link::ReadReply() {
  using clock = std::chrono::steady_clock;
  const auto deadline =
      clock::now() + std::chrono::milliseconds(opts_.reply_timeout_ms);

  uint8_t buf[512];
  std::string raw;

  while (clock::now() < deadline) {
    auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - clock::now())
                       .count();
    if (left_ms <= 0)
      break;
    uint32_t slice =
        static_cast<uint32_t>(std::min<int64_t>(left_ms, 50));

    size_t n = port_.ReadTimeout(buf, sizeof(buf), slice);
    if (n == 0)
      continue;

    raw.append(reinterpret_cast<char *>(buf), n);

    if (ExtractTaggedReply(raw).empty())
      continue;

    while (true) {
      size_t more = port_.ReadTimeout(buf, sizeof(buf), opts_.idle_gap_ms);
      if (more == 0)
        break;
      raw.append(reinterpret_cast<char *>(buf), more);
    }
    return ExtractTaggedReply(raw);
  }

  return ExtractTaggedReply(raw);
}

} // namespace rs485
