#include "cardlink/midi/midi_input.hpp"

#include <RtMidi.h>

#include <cctype>
#include <stdexcept>

namespace cardlink::midi
{
namespace
{

std::string ToLower(std::string s)
{
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

} // namespace

struct MidiInput::Impl
{
  RtMidiIn midiin;
};

MidiInput::MidiInput()
    : impl_(new Impl)
{
}

MidiInput::~MidiInput()
{
  Close();
  delete impl_;
  impl_ = nullptr;
}

std::vector<MidiPortInfo> MidiInput::ListPorts()
{
  RtMidiIn midiin;
  std::vector<MidiPortInfo> ports;
  const unsigned n = midiin.getPortCount();
  ports.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    MidiPortInfo info;
    info.index = i;
    info.name = midiin.getPortName(i);
    ports.push_back(info);
  }
  return ports;
}

void MidiInput::Open(std::optional<unsigned> port_index)
{
  Close();

  const unsigned n = impl_->midiin.getPortCount();
  if (n == 0) {
    throw std::runtime_error("no MIDI input ports found");
  }

  unsigned chosen = 0;
  if (port_index) {
    if (*port_index >= n) {
      throw std::runtime_error("MIDI port index out of range");
    }
    chosen = *port_index;
  } else {
    // Launchkey exposes two (or more) ports. Keyboard notes live on
    // "... MIDI Out"; "... DAW Out" is transport/CC for a DAW — skip it
    // when auto-picking.
    std::optional<unsigned> preferred; // Launchkey + MIDI, not DAW
    std::optional<unsigned> fallback;  // any other Launchkey

    for (unsigned i = 0; i < n; ++i) {
      const std::string lower = ToLower(impl_->midiin.getPortName(i));
      if (lower.find("launchkey") == std::string::npos) {
        continue;
      }
      const bool is_daw = lower.find("daw") != std::string::npos;
      const bool is_midi = lower.find("midi") != std::string::npos;
      if (is_midi && !is_daw) {
        preferred = i;
        break;
      }
      if (!fallback && !is_daw) {
        fallback = i;
      } else if (!fallback) {
        fallback = i;
      }
    }

    if (preferred) {
      chosen = *preferred;
    } else if (fallback) {
      chosen = *fallback;
    } else {
      throw std::runtime_error(
          "no Launchkey MIDI port found; use --list and --port <index>");
    }
  }

  impl_->midiin.setCallback(&MidiInput::RtMidiCallback, this);
  impl_->midiin.ignoreTypes(true, true, true); // ignore sysex, time, sense
  impl_->midiin.openPort(chosen);
  port_name_ = impl_->midiin.getPortName(chosen);
}

void MidiInput::Close()
{
  if (!impl_) {
    return;
  }
  if (impl_->midiin.isPortOpen()) {
    impl_->midiin.cancelCallback();
    impl_->midiin.closePort();
  }
  port_name_.clear();
  std::lock_guard<std::mutex> lock(queue_mu_);
  while (!queue_.empty()) {
    queue_.pop();
  }
}

void MidiInput::Poll(std::vector<NoteEvent>& out)
{
  out.clear();
  std::lock_guard<std::mutex> lock(queue_mu_);
  while (!queue_.empty()) {
    out.push_back(queue_.front());
    queue_.pop();
  }
}

void MidiInput::RtMidiCallback(double /*time_stamp*/,
                               std::vector<unsigned char>* message,
                               void* user_data)
{
  if (!message || !user_data) {
    return;
  }
  static_cast<MidiInput*>(user_data)->HandleMessage(*message);
}

std::optional<NoteEvent>
DecodeNoteMessage(const std::vector<unsigned char>& message)
{
  if (message.size() < 3) {
    return std::nullopt;
  }

  const uint8_t status = message[0];
  const uint8_t type = status & 0xF0u;
  const uint8_t data1 = message[1] & 0x7Fu;
  const uint8_t data2 = message[2] & 0x7Fu;

  NoteEvent ev;
  if (type == 0x90u) {
    // Note On with velocity 0 is Note Off (MIDI convention).
    ev.action = (data2 == 0) ? NoteAction::Off : NoteAction::On;
    ev.key = data1;
    ev.velocity = data2;
  } else if (type == 0x80u) {
    ev.action = NoteAction::Off;
    ev.key = data1;
    ev.velocity = 0;
  } else if (type == 0xB0u) {
    // CC 120 All Sound Off / CC 123 All Notes Off — kill stuck tones.
    if (data1 == 120 || data1 == 123) {
      ev.action = NoteAction::AllOff;
      ev.key = 0;
      ev.velocity = 0;
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  return ev;
}

void MidiInput::HandleMessage(const std::vector<unsigned char>& message)
{
  const std::optional<NoteEvent> decoded = DecodeNoteMessage(message);
  if (!decoded) {
    return;
  }
  std::lock_guard<std::mutex> lock(queue_mu_);
  queue_.push(*decoded);
}

} // namespace cardlink::midi
