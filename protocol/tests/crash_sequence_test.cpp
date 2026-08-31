#include "cardlink/sequence/crash_sequence.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

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
  std::istringstream text(
      "c4 200 2\n"
      "d4 200\n"
      "w 500\n"
      "repeat 3\n");
  cardlink::sequence::CrashSequence sequence;
  std::string error;
  Check(cardlink::sequence::ParseCrashSequence(text, sequence, error),
        error.c_str());
  Check(sequence.steps.size() == 3, "wrong step count");
  Check(sequence.repeat_count == 3 && !sequence.repeat_forever,
        "wrong repeat count");
  Check(std::fabs(sequence.steps[0].hz - 261.625565) < 0.001 &&
            sequence.steps[0].wave_id == 60 && sequence.steps[0].release &&
            sequence.steps[0].duration_ms == 200 &&
            sequence.steps[0].release_ms == 2,
        "wrong C4 note");
  Check(std::fabs(sequence.steps[1].hz - 293.664768) < 0.001 &&
            sequence.steps[1].wave_id == 62 && !sequence.steps[1].release,
        "wrong D4 note");
  Check(sequence.steps[2].duration_ms == 500, "wrong wait");

  std::istringstream release_suffix("c4 100 3ms\n");
  Check(cardlink::sequence::ParseCrashSequence(release_suffix, sequence,
                                                error) &&
            sequence.steps[0].release_ms == 3,
        "millisecond release suffix rejected");

  std::istringstream bad("c4 nope\n");
  Check(!cardlink::sequence::ParseCrashSequence(bad, sequence, error) &&
            error.find("line 1") != std::string::npos,
        "bad release accepted");

  return EXIT_SUCCESS;
}
