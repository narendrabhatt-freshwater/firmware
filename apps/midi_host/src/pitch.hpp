#pragma once

#include <cstdint>
#include <string>

namespace midi_host
{

/** Equal temperament: MIDI 69 = A4 = 440 Hz. */
double MidiNoteToHz(uint8_t midi_note);

/** Scientific pitch name, e.g. MIDI 69 → "A4", MIDI 60 → "C4". */
std::string MidiNoteName(uint8_t midi_note);

} // namespace midi_host
