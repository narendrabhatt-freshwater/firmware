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
  constexpr uint8_t kTypeStatus = 0x01;

  ExchangeResult out;
  out.from = Target::Channel;
  if (frame == nullptr || len != kVqBinaryFrameLen ||
      frame[0] != kSync0 || frame[1] != kSync1 ||
      frame[2] != kCardChannel || frame[3] != kTypeStatus ||
      frame[11] != '\n' || frame[10] != Crc8(frame, 10)) {
    out.status = Status::BadReply;
    std::snprintf(out.raw, sizeof(out.raw), "bad binary vq frame");
    return out;
  }

  std::array<uint8_t, 8> slots{};
  for (size_t i = 0; i < slots.size(); i += 2) {
    const uint8_t packed = frame[6 + i / 2];
    slots[i] = static_cast<uint8_t>(packed & 0x0Fu);
    slots[i + 1] = static_cast<uint8_t>((packed >> 4u) & 0x0Fu);
    if (slots[i] > 15u || slots[i + 1] > 15u) {
      out.status = Status::BadReply;
      std::snprintf(out.raw, sizeof(out.raw), "bad binary vq slots");
      return out;
    }
  }

  out.status = Status::Ok;
  std::snprintf(out.raw, sizeof(out.raw),
                "ok:vq %02x %u %u %u %u %u %u %u %u %u",
                static_cast<unsigned>(frame[4]),
                static_cast<unsigned>(frame[5]),
                static_cast<unsigned>(slots[0]),
                static_cast<unsigned>(slots[1]),
                static_cast<unsigned>(slots[2]),
                static_cast<unsigned>(slots[3]),
                static_cast<unsigned>(slots[4]),
                static_cast<unsigned>(slots[5]),
                static_cast<unsigned>(slots[6]),
                static_cast<unsigned>(slots[7]));
  return out;
}

} // namespace rs485
} // namespace cardlink
