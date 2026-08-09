#include "pitch.hpp"

#include <cmath>
#include <cstdio>

namespace midi_host
{

double MidiNoteToHz(uint8_t midi_note)
{
  // f = 440 * 2^((n - 69) / 12)
  return 440.0 * std::pow(2.0, (static_cast<double>(midi_note) - 69.0) / 12.0);
}

uint8_t HzToNearestMidi(double hz)
{
  if (hz <= 0.0) {
    return 0;
  }
  /* n = 69 + 12 * log2(f / 440) */
  const double n = 69.0 + 12.0 * (std::log(hz / 440.0) / std::log(2.0));
  int note = static_cast<int>(std::lround(n));
  if (note < 0) {
    note = 0;
  }
  if (note > 127) {
    note = 127;
  }
  return static_cast<uint8_t>(note);
}

std::string MidiNoteName(uint8_t midi_note)
{
  static const char* const kNames[12] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  // MIDI 0 = C-1, MIDI 12 = C0, MIDI 60 = C4, MIDI 69 = A4.
  const int octave = static_cast<int>(midi_note / 12) - 1;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%s%d", kNames[midi_note % 12], octave);
  return std::string(buf);
}

} // namespace midi_host
