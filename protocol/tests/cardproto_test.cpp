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
  Check(cardproto::FormatSetStreamNote(3, 440.0, 17u) ==
            "n3 440.000000000 0.125000000 @17",
        "session-bound note formatting changed");
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

  cardproto::VoiceQuery query;
  Check(cardproto::ParseVoiceQuery(
            "ok:vq 81 7 0 1 255 256 1024 2048 4096 12240 4660", query) &&
            query.mask == 0x81u && query.best == 7u &&
            query.free_samples[0] == 0u &&
            query.free_samples[7] == 12240u &&
            query.last_pack_sequence == 4660u,
        "exact-credit vq parsing changed");
  Check(!cardproto::ParseVoiceQuery(
            "ok:vq 01 0 12241 0 0 0 0 0 0 0 0", query),
        "vq must reject free space beyond the physical ring");

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
