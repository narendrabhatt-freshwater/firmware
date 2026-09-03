#pragma once

#include <cstdint>

namespace cardlink::midi
{

enum class NoteAction : uint8_t
{
  On = 0,
  Off = 1,
  AllOff = 2, // MIDI CC 120/123 — release every voice
};

/** MIDI note on/off. Note On velocity is 1..127; other actions use zero. */
struct NoteEvent
{
  NoteAction action = NoteAction::Off;
  uint8_t key = 0; // 0..127
  uint8_t velocity = 0; // 1..127 for NoteAction::On
};

} // namespace cardlink::midi
