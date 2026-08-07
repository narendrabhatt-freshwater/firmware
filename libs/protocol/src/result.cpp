#include "protocol/result.hpp"

#include <cstdio>
#include <cstring>

namespace protocol
{
  namespace
  {

    void CopyCapped(char *dst, size_t dst_n, const char *src)
    {
      if (!dst || dst_n == 0)
      {
        return;
      }
      if (!src)
      {
        dst[0] = '\0';
        return;
      }
      std::snprintf(dst, dst_n, "%s", src);
    }

  } // namespace

  Result Result::LocalErr(const char *code, const char *detail)
  {
    Result r;
    r.status = Status::Err;
    CopyCapped(r.err_code, sizeof(r.err_code), code ? code : "range");
    CopyCapped(r.raw, sizeof(r.raw), detail ? detail : "");
    return r;
  }

  Result Result::IoErr(const char *detail)
  {
    Result r;
    r.status = Status::IoError;
    CopyCapped(r.raw, sizeof(r.raw), detail ? detail : "io");
    return r;
  }

  Result ParseReplyBody(const std::string &line, Target expected)
  {
    std::string body = line;
    Target from = expected;

    if (body.rfind("[C]", 0) == 0)
    {
      from = Target::Channel;
      body.erase(0, 3);
      if (!body.empty() && body[0] == ' ')
      {
        body.erase(0, 1);
      }
    }
    else if (body.rfind("[E]", 0) == 0)
    {
      from = Target::Effect;
      body.erase(0, 3);
      if (!body.empty() && body[0] == ' ')
      {
        body.erase(0, 1);
      }
    }

    Result r;
    r.from = from;
    CopyCapped(r.raw, sizeof(r.raw), body.c_str());

    if (body.rfind("ok", 0) == 0 &&
        (body.size() == 2 || body[2] == ':' || body[2] == '\0'))
    {
      r.status = Status::Ok;
      return r;
    }
    if (body.rfind("err:", 0) == 0)
    {
      r.status = Status::Err;
      const std::string code = body.substr(4);
      const size_t sp = code.find(' ');
      const std::string token =
          (sp == std::string::npos) ? code : code.substr(0, sp);
      CopyCapped(r.err_code, sizeof(r.err_code), token.c_str());
      return r;
    }

    r.status = Status::BadReply;
    return r;
  }

} // namespace protocol
