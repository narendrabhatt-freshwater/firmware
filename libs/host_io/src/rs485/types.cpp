#include "host_io/rs485/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace host_io {
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
  CopyTrunc(r.raw, sizeof(r.raw), line_in);

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

  /* Skip "[X]" and optional space. */
  size_t i = 3;
  while (i < line.size() && line[i] == ' ') {
    ++i;
  }
  const std::string body = line.substr(i);
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

} // namespace rs485
} // namespace host_io
