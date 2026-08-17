#include "cardproto/channel.hpp"
#include "cardproto/effect.hpp"
#include "cardproto/result.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{

void Check(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

int main()
{
  Check(cardproto::FormatSetNote(0, 261.625565) == "n0 261.625565000",
        "note formatting changed");
  Check(cardproto::FormatSetNote(7, 0.0) == "n7 0",
        "note-off formatting changed");
  Check(cardproto::FormatSetAllNotes(440.0, 0.125) ==
            "n 440.000000000 0.125000000",
        "all-note formatting changed");
  Check(cardproto::FormatSet48V(true) == "v 1",
        "effect formatting changed");

  const auto ok =
      cardproto::ParseReplyBody("[C] ok:vq ff 0 1 1 1 1 1 1 1 1",
                                cardproto::Target::Effect);
  Check(ok.status == cardproto::Status::Ok &&
            ok.from == cardproto::Target::Channel &&
            std::strcmp(ok.raw, "ok:vq ff 0 1 1 1 1 1 1 1 1") == 0,
        "tagged ok reply parsing changed");

  const auto err =
      cardproto::ParseReplyBody("[E]err:range detail",
                                cardproto::Target::Channel);
  Check(err.status == cardproto::Status::Err &&
            err.from == cardproto::Target::Effect &&
            std::strcmp(err.err_code, "range") == 0,
        "tagged error reply parsing changed");

  const auto bad =
      cardproto::ParseReplyBody("unexpected", cardproto::Target::Channel);
  Check(bad.status == cardproto::Status::BadReply,
        "unknown reply must remain a bad reply");

  return EXIT_SUCCESS;
}
