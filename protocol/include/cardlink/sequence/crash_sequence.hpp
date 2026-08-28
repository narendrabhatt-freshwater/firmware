#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace cardlink::sequence
{

enum class CrashStepKind : uint8_t {
  Note,
  Delay,
  Command,
  Envelope,
  CrashRelease,
};

struct CrashStep
{
  CrashStepKind kind = CrashStepKind::Delay;
  unsigned line = 0;
  double hz = 0.0;
  uint16_t wave_id = 0;
  uint32_t duration_ms = 0;
  bool release = false;
  uint32_t release_ms = 0;
  std::string command;
  std::string envelope;
  std::string label;
};

struct CrashSequence
{
  std::vector<CrashStep> steps;
  uint32_t repeat_count = 1;
  bool repeat_forever = false;
};

/** Parse the hand-editable crash sequence format documented in README.md. */
bool ParseCrashSequence(std::istream &input, CrashSequence &out,
                        std::string &error);

/** Open and parse a crash sequence file. */
bool LoadCrashSequence(const std::string &path, CrashSequence &out,
                       std::string &error);

} // namespace cardlink::sequence
