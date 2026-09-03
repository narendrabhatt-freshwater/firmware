#include "cardlink/rs485/link.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

namespace cardlink {
namespace rs485 {
namespace {

constexpr uint8_t kVqSync0 = 0xA5;
constexpr uint8_t kVqSync1 = 0x5A;

bool FindVqFrame(const std::string &raw,
                 std::array<uint8_t, kVqBinaryFrameLen> &frame) {
  if (raw.size() < kVqBinaryFrameLen) {
    return false;
  }
  for (size_t i = 0; i + kVqBinaryFrameLen <= raw.size(); ++i) {
    if (static_cast<uint8_t>(raw[i]) != kVqSync0 ||
        static_cast<uint8_t>(raw[i + 1]) != kVqSync1) {
      continue;
    }
    for (size_t j = 0; j < kVqBinaryFrameLen; ++j) {
      frame[j] = static_cast<uint8_t>(raw[i + j]);
    }
    return true;
  }
  return false;
}

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

/** First tagged line ending with CRLF, LF, or bare CR (USB-serial quirk). */
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
  size_t cr = tagged.find('\r');
  if (cr != std::string::npos) {
    return tagged.substr(0, cr);
  }
  return {};
}

void SleepMs(uint32_t ms) {
  if (ms == 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

Link::Link(cardlink::SerialPort &port, LinkOptions opts) : port_(port), opts_(opts) {}

bool Link::WriteWire(const uint8_t *bytes, size_t len) {
  if (port_.ManualRtsControl()) {
    port_.SetRts(true);
  }
  const bool ok = port_.Write(bytes, len);
  if (ok) {
    port_.DrainOutput();
  }
  /* Release DE only after the last stop bit — never mid-frame. */
  if (port_.ManualRtsControl()) {
    port_.SetRts(false);
  }
  return ok;
}

void Link::WaitRxIdle() {
  using clock = std::chrono::steady_clock;
  if (opts_.rx_idle_ms == 0) {
    return;
  }
  const auto deadline =
      clock::now() + std::chrono::milliseconds(opts_.rx_idle_max_ms);
  auto last_rx = clock::now();
  uint8_t buf[128];

  while (clock::now() < deadline) {
    auto since_rx = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock::now() - last_rx)
                        .count();
    if (since_rx >= static_cast<int64_t>(opts_.rx_idle_ms)) {
      return;
    }
    auto left_idle = static_cast<uint32_t>(
        opts_.rx_idle_ms - static_cast<uint32_t>(since_rx));
    auto left_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - clock::now())
                          .count();
    if (left_total <= 0) {
      return;
    }
    uint32_t slice = static_cast<uint32_t>(
        std::min<int64_t>(left_idle, std::min<int64_t>(left_total, 20)));
    size_t n = port_.ReadTimeout(buf, sizeof(buf), slice);
    if (n > 0) {
      last_rx = clock::now();
    }
  }
}

std::string Link::ReadRawWindow(bool accept_vq_binary) {
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

    if (accept_vq_binary) {
      std::array<uint8_t, kVqBinaryFrameLen> frame{};
      if (FindVqFrame(raw, frame)) {
        return raw;
      }
    }

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
  /* Timeout: return whatever arrived (may be empty or untagged noise). */
  return raw;
}

ExchangeResult Link::ReadTerminalReply(Target expected,
                                       bool accept_vq_binary) {
  std::string window = ReadRawWindow(accept_vq_binary);
  if (accept_vq_binary) {
    std::array<uint8_t, kVqBinaryFrameLen> frame{};
    if (FindVqFrame(window, frame)) {
      return ParseVqBinaryReply(frame.data(), frame.size());
    }
    const auto sync = window.find(std::string("\xA5\x5A\x43", 3));
    if (sync != std::string::npos && window.size() > sync + 3u &&
        static_cast<uint8_t>(window[sync + 3u]) != 0x04u) {
      ExchangeResult r;
      r.status = Status::BadReply;
      r.from = Target::Channel;
      std::snprintf(r.raw, sizeof(r.raw),
                    "firmware/host mismatch: Sample mode requires ABI1 vq3");
      return r;
    }
  }
  std::string tagged = ExtractTaggedRegion(window);
  if (tagged.empty()) {
    ExchangeResult r;
    r.status = Status::Timeout;
    /* Sanitize for logs — show if RX was empty vs garbage. */
    std::string shown;
    shown.reserve(window.size());
    for (unsigned char c : window) {
      if (c == '\r') {
        shown += "\\r";
      } else if (c == '\n') {
        shown += "\\n";
      } else if (c >= 0x20 && c < 0x7f && c != '%') {
        shown.push_back(static_cast<char>(c));
      } else {
        shown.push_back('.');
      }
    }
    if (shown.empty()) {
      shown = "(empty)";
    }
    const size_t n = std::min(shown.size(), sizeof(r.raw) - 1);
    std::memcpy(r.raw, shown.data(), n);
    r.raw[n] = '\0';
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
  const std::string body = explicit_prefix ? command.substr(2) : command;

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
  const bool vq_binary = expect == Target::Channel && body == "vq";

  ExchangeResult result;
  result.status = Status::Timeout;

  for (int attempt = 0; attempt <= opts_.retries; ++attempt) {
    port_.FlushInput();

    const auto *bytes = reinterpret_cast<const uint8_t *>(wire.data());
    if (!WriteWire(bytes, wire.size())) {
      result.status = Status::IoError;
      last_ = result;
      return result;
    }

    SleepMs(opts_.post_tx_settle_ms);

    result = ReadTerminalReply(expect, vq_binary);

    if (result.status == Status::Ok || result.status == Status::Err) {
      if (result.status == Status::Err) {
        ++err_count_;
      }
      if (!vq_binary && opts_.rx_idle_ms > 0) {
        WaitRxIdle();
      }
      SleepMs(opts_.post_ack_settle_ms);
      last_ = result;
      return result;
    }
    if (result.status == Status::IoError) {
      last_ = result;
      return result;
    }

    /* Late ACK peek (even when retries==0), then idle before resend. */
    if (opts_.late_ack_grace_ms > 0) {
      const uint32_t saved_to = opts_.reply_timeout_ms;
      opts_.reply_timeout_ms = opts_.late_ack_grace_ms;
      ExchangeResult late = ReadTerminalReply(expect, vq_binary);
      opts_.reply_timeout_ms = saved_to;
      if (late.status == Status::Ok || late.status == Status::Err) {
        if (late.status == Status::Err) {
          ++err_count_;
        }
        if (!vq_binary && opts_.rx_idle_ms > 0) {
          WaitRxIdle();
        }
        SleepMs(opts_.post_ack_settle_ms);
        last_ = late;
        return late;
      }
    }
    if (attempt < opts_.retries && opts_.rx_idle_ms > 0) {
      WaitRxIdle();
    }
  }

  if (opts_.rx_idle_ms > 0) {
    WaitRxIdle();
  }

  if (result.status == Status::Timeout || result.status == Status::BadReply) {
    ++timeout_count_;
  }
  last_ = result;
  return result;
}

bool Link::SendBlind(Target target, const std::string &command_in) {
  std::string command = ToLower(command_in);
  bool explicit_prefix = command.size() >= 2 && command[1] == ':';
  std::string line =
      explicit_prefix ? command : (TargetPrefix(target) + command);
  std::string wire = line + "\r";

  port_.FlushInput();
  const auto *bytes = reinterpret_cast<const uint8_t *>(wire.data());
  if (!WriteWire(bytes, wire.size())) {
    return false;
  }
  SleepMs(opts_.post_tx_settle_ms);
  WaitRxIdle();
  port_.FlushInput();
  return true;
}

} // namespace rs485
} // namespace cardlink
