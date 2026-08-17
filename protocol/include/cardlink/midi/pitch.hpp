#pragma once

#include <cstdint>
#include <string>

namespace cardlink::midi
{

/** Equal temperament: MIDI 69 = A4 = 440 Hz. */
double MidiNoteToHz(uint8_t midi_note);

/**
 * Nearest MIDI note for a frequency (display / local preview only).
 * Clamps to 0..127. Non-positive hz → 0.
 */
uint8_t HzToNearestMidi(double hz);

/** Scientific pitch name, e.g. MIDI 69 → "A4", MIDI 60 → "C4". */
std::string MidiNoteName(uint8_t midi_note);

} // namespace cardlink::midi
