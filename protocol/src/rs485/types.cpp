#include "cardlink/rs485/types.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace cardlink {
namespace rs485 {
namespace {

std::string ToLower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

void CopyTrunc(char *dst, size_t dst_sz, const std::string &src) {
  if (dst_sz == 0) {
    return;
  }
  const size_t n = std::min(src.size(), dst_sz - 1);
  if (n > 0) {
    std::memcpy(dst, src.data(), n);
  }
  dst[n] = '\0';
}

uint8_t Crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80u) != 0u
                ? static_cast<uint8_t>((crc << 1u) ^ 0x07u)
                : static_cast<uint8_t>(crc << 1u);
    }
  }
  return crc;
}

} // namespace

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

ExchangeResult ParseTaggedReply(const std::string &line_in, Target expected) {
  ExchangeResult r;
  std::string line = line_in;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                           line.back() == ' ')) {
    line.pop_back();
  }
  CopyTrunc(r.raw, sizeof(r.raw), line);

  if (line.size() < 4 || line[0] != '[' || line[2] != ']') {
    r.status = Status::BadReply;
    return r;
  }

  const char tag = static_cast<char>(std::toupper(static_cast<unsigned char>(line[1])));
  if (tag == 'C') {
    r.from = Target::Channel;
  } else if (tag == 'E') {
    r.from = Target::Effect;
  } else {
    r.status = Status::BadReply;
    return r;
  }

  if (expected != Target::All && r.from != expected) {
    r.status = Status::BadReply;
    return r;
  }

  /* Skip "[X]" and optional space. raw is the body — same contract as
   * cardproto::ParseReplyBody / Result::raw. */
  size_t i = 3;
  while (i < line.size() && line[i] == ' ') {
    ++i;
  }
  const std::string body = line.substr(i);
  CopyTrunc(r.raw, sizeof(r.raw), body);
  const std::string body_l = ToLower(body);

  if (body_l == "ok" || body_l.rfind("ok:", 0) == 0 ||
      body_l.rfind("ok ", 0) == 0) {
    r.status = Status::Ok;
    return r;
  }

  if (body_l.rfind("err:", 0) == 0) {
    r.status = Status::Err;
    std::string code = body.substr(4);
    /* First token only. */
    size_t sp = code.find_first_of(" \t\r\n");
    if (sp != std::string::npos) {
      code = code.substr(0, sp);
    }
    CopyTrunc(r.err_code, sizeof(r.err_code), ToLower(code));
    return r;
  }

  /* Maintenance multi-line / status: treat as Ok if we got a tagged line. */
  r.status = Status::Ok;
  return r;
}

ExchangeResult ParseVqBinaryReply(const uint8_t *frame, size_t len) {
  constexpr uint8_t kSync0 = 0xA5;
  constexpr uint8_t kSync1 = 0x5A;
  constexpr uint8_t kCardChannel = 0x43;
  constexpr uint8_t kTypeStatus = 0x04;

  ExchangeResult out;
  out.from = Target::Channel;
  if (frame == nullptr || len != kVqBinaryFrameLen ||
      frame[0] != kSync0 || frame[1] != kSync1 ||
      frame[2] != kCardChannel || frame[3] != kTypeStatus ||
      frame[55] != '\n' || frame[54] != Crc8(frame, 54)) {
    out.status = Status::BadReply;
    std::snprintf(out.raw, sizeof(out.raw), "bad binary vq frame");
    return out;
  }

  const uint16_t capacity = static_cast<uint16_t>(frame[8]) |
                            static_cast<uint16_t>(frame[9] << 8u);
  const uint16_t status_sequence = static_cast<uint16_t>(frame[10]) |
                                   static_cast<uint16_t>(frame[11] << 8u);
  const uint16_t uac_sequence = static_cast<uint16_t>(frame[12]) |
                                static_cast<uint16_t>(frame[13] << 8u);
  std::array<uint8_t, 8> sessions{};
  std::array<uint16_t, 8> fills{};
  std::array<uint16_t, 8> free_samples{};
  if (capacity == 0u) {
    out.status = Status::BadReply;
    std::snprintf(out.raw, sizeof(out.raw), "bad binary vq capacity");
    return out;
  }
  for (size_t i = 0; i < free_samples.size(); ++i) {
    const size_t at = 14u + 5u * i;
    sessions[i] = frame[at];
    fills[i] = static_cast<uint16_t>(frame[at + 1u]) |
               static_cast<uint16_t>(frame[at + 2u] << 8u);
    free_samples[i] = static_cast<uint16_t>(frame[at + 3u]) |
                      static_cast<uint16_t>(frame[at + 4u] << 8u);
    if (fills[i] > capacity || free_samples[i] > capacity ||
        static_cast<uint32_t>(fills[i]) + free_samples[i] > capacity) {
      out.status = Status::BadReply;
      std::snprintf(out.raw, sizeof(out.raw), "bad binary vq credit");
      return out;
    }
  }
  out.status = Status::Ok;
  int n = std::snprintf(out.raw, sizeof(out.raw),
                        "ok:vq7 %02x %02x %u %u %u %u",
                        static_cast<unsigned>(frame[4]),
                        static_cast<unsigned>(frame[5]),
                        static_cast<unsigned>(frame[6]),
                        static_cast<unsigned>(capacity),
                        static_cast<unsigned>(status_sequence),
                        static_cast<unsigned>(uac_sequence));
  for (size_t i = 0; i < 8u && n > 0 &&
       static_cast<size_t>(n) < sizeof(out.raw); ++i) {
    n += std::snprintf(out.raw + n, sizeof(out.raw) - static_cast<size_t>(n),
                       " %u %u %u", static_cast<unsigned>(sessions[i]),
                       static_cast<unsigned>(fills[i]),
                       static_cast<unsigned>(free_samples[i]));
  }
  return out;
}

} // namespace rs485
} // namespace cardlink
