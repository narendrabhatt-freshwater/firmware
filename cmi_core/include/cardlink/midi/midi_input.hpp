#pragma once

#include "cardlink/midi/note_event.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace cardlink::midi
{

struct MidiPortInfo
{
  unsigned index = 0;
  std::string name;
};

/** Decode a MIDI note/all-notes-off message; returns empty for other input. */
std::optional<NoteEvent>
DecodeNoteMessage(const std::vector<unsigned char>& message);

/**
 * RtMidiIn wrapper: open Launchkey (or --port), queue NoteEvents.
 * MIDI callback only enqueues; main thread drains via Poll().
 */
class MidiInput
{
public:
  MidiInput();
  ~MidiInput();

  MidiInput(const MidiInput&) = delete;
  MidiInput& operator=(const MidiInput&) = delete;

  static std::vector<MidiPortInfo> ListPorts();

  /**
   * Open a port. If port_index is set, use it; else first name containing
   * "Launchkey" (case-insensitive). Throws on failure.
   */
  void Open(std::optional<unsigned> port_index = std::nullopt);

  void Close();

  /** Drain queued note events into out (clears queue). */
  void Poll(std::vector<NoteEvent>& out);

  std::string PortName() const { return port_name_; }

private:
  static void RtMidiCallback(double time_stamp,
                             std::vector<unsigned char>* message,
                             void* user_data);

  void HandleMessage(const std::vector<unsigned char>& message);

  struct Impl;
  Impl* impl_ = nullptr;

  std::mutex queue_mu_;
  std::queue<NoteEvent> queue_;
  std::string port_name_;
};

} // namespace cardlink::midi
