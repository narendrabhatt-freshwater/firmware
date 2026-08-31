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
  Check(cardproto::FormatNoteOn(0, 60u) == "n0 on 60",
        "note formatting changed");
  Check(cardproto::FormatNoteOn(7, 0u) == "n7 on 0",
        "lowest-key formatting changed");
  Check(cardproto::FormatNoteOff(0) == "n0 off" &&
            cardproto::FormatNoteOff(7) == "n7 off",
        "note-off formatting changed");
  Check(cardproto::FormatStreamNoteOn(3, 69u, 17u) ==
            "n3 on 69 @17",
        "session-bound note formatting changed");
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
            "ok:vq7 81 80 7 12240 4660 43981 1 508 11732 2 0 12240 3 0 12240 4 0 12240 5 0 12240 6 0 12240 7 0 12240 8 0 12240", query) &&
            query.active_mask == 0x81u && query.pending_mask == 0x80u &&
            query.best == 7u && query.target_session[0] == 1u &&
            query.target_fill[0] == 508u && query.free_samples[0] == 11732u &&
            query.status_sequence == 4660u && query.uac_sequence == 43981u,
        "exact-credit vq parsing changed");
  Check(!cardproto::ParseVoiceQuery(
            "ok:vq 01 0 8161 0 0 0 0 0 0 0 0", query),
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
