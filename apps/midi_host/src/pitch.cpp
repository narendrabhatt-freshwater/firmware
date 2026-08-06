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
