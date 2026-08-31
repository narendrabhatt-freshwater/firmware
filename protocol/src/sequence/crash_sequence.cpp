#include "cardlink/sequence/crash_sequence.hpp"

#include "cardlink/midi/pitch.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace cardlink::sequence
{
namespace
{

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

bool ParseUnsigned(const std::string &text, uint32_t &value)
{
  if (text.empty() || text[0] == '-') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool ParseMs(std::string text, uint32_t &value)
{
  const std::string lower = Lower(text);
  if (lower.size() > 2 && lower.substr(lower.size() - 2) == "ms") {
    text.resize(text.size() - 2);
  }
  return ParseUnsigned(text, value);
}

std::string Join(const std::vector<std::string> &tokens)
{
  std::string command;
  for (const auto &token : tokens) {
    if (!command.empty()) {
      command += ' ';
    }
    command += token;
  }
  return command;
}

bool ParsePitch(const std::string &text, double &hz)
{
  char *end = nullptr;
  errno = 0;
  const double numeric = std::strtod(text.c_str(), &end);
  if (errno == 0 && end != text.c_str() && *end == '\0') {
    if (numeric >= 20.0 && numeric < 20000.0 && std::isfinite(numeric)) {
      hz = numeric;
      return true;
    }
    return false;
  }

  if (text.size() < 2 || text.size() > 4) {
    return false;
  }
  const std::string names = "C D EF G A B";
  const char letter = static_cast<char>(
      std::toupper(static_cast<unsigned char>(text[0])));
  const auto base = names.find(letter);
  if (base == std::string::npos) {
    return false;
  }
  int semitone = static_cast<int>(base);
  size_t pos = 1;
  if (pos < text.size() && (text[pos] == '#' || text[pos] == 'b')) {
    semitone += text[pos] == '#' ? 1 : -1;
    ++pos;
  }
  if (pos >= text.size()) {
    return false;
  }
  char *octave_end = nullptr;
  const long octave = std::strtol(text.c_str() + pos, &octave_end, 10);
  if (*octave_end != '\0') {
    return false;
  }
  const long midi = (octave + 1) * 12 + semitone;
  if (midi < 0 || midi > 127) {
    return false;
  }
  hz = cardlink::midi::MidiNoteToHz(static_cast<uint8_t>(midi));
  return hz >= 20.0 && hz < 20000.0;
}

bool Fail(unsigned line, const std::string &message, std::string &error)
{
  error = "line " + std::to_string(line) + ": " + message;
  return false;
}

} // namespace

bool ParseCrashSequence(std::istream &input, CrashSequence &out,
                        std::string &error)
{
  CrashSequence parsed;
  std::string line;
  unsigned line_number = 0;
  bool saw_repeat = false;

  while (std::getline(input, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    std::istringstream words(line);
    std::vector<std::string> token;
    for (std::string word; words >> word;) {
      token.push_back(std::move(word));
    }
    if (token.empty()) {
      continue;
    }
    if (saw_repeat) {
      return Fail(line_number, "repeat must be the final instruction", error);
    }

    const std::string op = Lower(token[0]);
    if (op == "repeat") {
      if (parsed.steps.empty()) {
        return Fail(line_number, "nothing to repeat", error);
      }
      if (token.size() == 1) {
        parsed.repeat_forever = true;
      } else if (token.size() == 2 &&
                 ParseUnsigned(token[1], parsed.repeat_count) &&
                 parsed.repeat_count > 0) {
        parsed.repeat_forever = false;
      } else {
        return Fail(line_number, "expected `repeat` or `repeat COUNT`", error);
      }
      saw_repeat = true;
      continue;
    }

    CrashStep step;
    step.line = line_number;
    if (op == "w" || op == "delay" || op == "wait") {
      if (token.size() != 2 || !ParseMs(token[1], step.duration_ms)) {
        return Fail(line_number, "expected `w MILLISECONDS`", error);
      }
      step.kind = CrashStepKind::Delay;
      parsed.steps.push_back(std::move(step));
      continue;
    }
    if (op == "crash") {
      uint32_t ms = 0;
      if (token.size() != 2 || !ParseMs(token[1], ms) || ms > 50) {
        return Fail(line_number, "crash release must be 0..50ms", error);
      }
      step.kind = CrashStepKind::CrashRelease;
      step.release_ms = static_cast<uint8_t>(ms);
      parsed.steps.push_back(std::move(step));
      continue;
    }
    size_t pitch_index = op == "note" ? 1 : 0;
    double parsed_hz = 0.0;
    if (pitch_index >= token.size() ||
        !ParsePitch(token[pitch_index], parsed_hz)) {
      step.kind = CrashStepKind::Command;
      step.command = Join(token);
      parsed.steps.push_back(std::move(step));
      continue;
    }
    if (token.size() < pitch_index + 2 || token.size() > pitch_index + 3) {
      return Fail(line_number, "expected `NOTE HOLD_MS [STEAL_MS]`",
                  error);
    }
    step.kind = CrashStepKind::Note;
    step.label = token[pitch_index];
    step.hz = parsed_hz;
    step.wave_id = cardlink::midi::HzToNearestMidi(step.hz);
    if (!ParseMs(token[pitch_index + 1], step.duration_ms)) {
      return Fail(line_number, "invalid note duration `" +
                                   token[pitch_index + 1] + "`", error);
    }
    if (token.size() == pitch_index + 3) {
      uint32_t ms = 0;
      if (!ParseMs(token[pitch_index + 2], ms) || ms > 50u) {
        return Fail(line_number, "voice steal must be 0..50 milliseconds",
                    error);
      }
      step.release = true;
      step.release_ms = ms;
    }
    parsed.steps.push_back(std::move(step));
  }

  if (!input.eof() && input.fail()) {
    error = "could not read sequence";
    return false;
  }
  if (parsed.steps.empty()) {
    error = "sequence is empty";
    return false;
  }
  out = std::move(parsed);
  error.clear();
  return true;
}

bool LoadCrashSequence(const std::string &path, CrashSequence &out,
                       std::string &error)
{
  std::ifstream input(path);
  if (!input) {
    error = "cannot open `" + path + "`";
    return false;
  }
  return ParseCrashSequence(input, out, error);
}

} // namespace cardlink::sequence
