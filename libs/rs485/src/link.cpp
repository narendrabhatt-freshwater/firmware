#include "rs485/link.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace rs485 {
namespace {

std::string ToLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool IsConsoleTextByte(unsigned char c) {
  return c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f);
}

/** Keep from first [C]/[E] tag; trim trailing non-text. */
std::string ExtractTaggedRegion(const std::string &raw) {
  size_t c = raw.find("[C]");
  size_t e = raw.find("[E]");
  size_t at = std::string::npos;
  if (c != std::string::npos) {
    at = c;
  }
  if (e != std::string::npos && (at == std::string::npos || e < at)) {
    at = e;
  }
  if (at == std::string::npos) {
    return {};
  }
  std::string out = raw.substr(at);
  while (!out.empty() &&
         !IsConsoleTextByte(static_cast<unsigned char>(out.back()))) {
    out.pop_back();
  }
  return out;
}

/** First tagged line ending with \r\n (or \n after tag). */
std::string FirstTerminalLine(const std::string &tagged) {
  if (tagged.empty()) {
    return {};
  }
  size_t crlf = tagged.find("\r\n");
  if (crlf != std::string::npos) {
    return tagged.substr(0, crlf);
  }
  size_t lf = tagged.find('\n');
  if (lf != std::string::npos) {
    return tagged.substr(0, lf);
  }
  return {};
}

} // namespace

Link::Link(SerialPort &port, LinkOptions opts) : port_(port), opts_(opts) {}

bool Link::WriteWire(const uint8_t *bytes, size_t len) {
  if (port_.ManualRtsControl()) {
    port_.SetRts(true);
  }
  const bool ok = port_.Write(bytes, len);
  if (port_.ManualRtsControl()) {
    port_.SetRts(false);
  }
  return ok;
}

std::string Link::ReadRawWindow() {
  using clock = std::chrono::steady_clock;
  const auto deadline =
      clock::now() + std::chrono::milliseconds(opts_.reply_timeout_ms);

  uint8_t buf[512];
  std::string raw;

  while (clock::now() < deadline) {
    auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - clock::now())
                       .count();
    if (left_ms <= 0) {
      break;
    }
    uint32_t slice =
        static_cast<uint32_t>(std::min<int64_t>(left_ms, 40));

    size_t n = port_.ReadTimeout(buf, sizeof(buf), slice);
    if (n == 0) {
      continue;
    }
    raw.append(reinterpret_cast<char *>(buf), n);

    std::string tagged = ExtractTaggedRegion(raw);
    if (tagged.empty()) {
      continue;
    }

    if (!FirstTerminalLine(tagged).empty()) {
      while (true) {
        size_t more =
            port_.ReadTimeout(buf, sizeof(buf), opts_.idle_gap_ms);
        if (more == 0) {
          break;
        }
        raw.append(reinterpret_cast<char *>(buf), more);
      }
      return ExtractTaggedRegion(raw);
    }
  }
  return ExtractTaggedRegion(raw);
}

ExchangeResult Link::ReadTerminalReply(Target expected) {
  std::string tagged = ReadRawWindow();
  if (tagged.empty()) {
    ExchangeResult r;
    r.status = Status::Timeout;
    return r;
  }

  std::string line = FirstTerminalLine(tagged);
  if (line.empty()) {
    line = tagged;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
  }

  if (!opts_.enforce_tag) {
    expected = Target::All;
  }
  return ParseTaggedReply(line, expected);
}

ExchangeResult Link::Send(Target target, const std::string &command_in) {
  std::string command = ToLower(command_in);

  bool explicit_prefix = command.size() >= 2 && command[1] == ':';
  std::string line =
      explicit_prefix ? command : (TargetPrefix(target) + command);
  std::string wire = line + "\r";

  Target expect = target;
  if (explicit_prefix) {
    if (command[0] == 'c') {
      expect = Target::Channel;
    } else if (command[0] == 'e') {
      expect = Target::Effect;
    } else {
      expect = Target::All;
    }
  }

  ExchangeResult result;
  result.status = Status::Timeout;

  for (int attempt = 0; attempt <= opts_.retries; ++attempt) {
    port_.FlushInput();

    const auto *bytes = reinterpret_cast<const uint8_t *>(wire.data());
    bool ok = WriteWire(bytes, wire.size());
    if (ok) {
      port_.DrainOutput();
    }

    if (!ok) {
      result.status = Status::IoError;
      last_ = result;
      return result;
    }

    result = ReadTerminalReply(expect);

    if (result.status == Status::Ok) {
      last_ = result;
      return result;
    }
    if (result.status == Status::Err) {
      ++err_count_;
      last_ = result;
      return result;
    }
    if (result.status == Status::IoError) {
      last_ = result;
      return result;
    }
  }

  if (result.status == Status::Timeout || result.status == Status::BadReply) {
    ++timeout_count_;
  }
  last_ = result;
  return result;
}

} // namespace rs485
