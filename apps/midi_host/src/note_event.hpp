#pragma once

#include <cstdint>

namespace midi_host
{

enum class NoteAction : uint8_t
{
  On = 0,
  Off = 1,
};

/** MIDI note on/off — key only; velocity is intentionally unused. */
struct NoteEvent
{
  NoteAction action = NoteAction::Off;
  uint8_t key = 0; // 0..127
};

} // namespace midi_host
