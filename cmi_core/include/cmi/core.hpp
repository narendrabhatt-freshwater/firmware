#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cmi {

enum class ErrorCode : uint8_t {
  Ok = 0,
  InvalidArgument,
  NotConnected,
  NotReady,
  CardError,
  Timeout,
  IoError,
  BadReply,
  VmError,
  SampleError,
  MidiError,
  AudioError,
};

struct Result {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  /** Card reply retained for logging; empty for local validation errors. */
  std::string reply;

  bool ok() const { return code == ErrorCode::Ok; }
  explicit operator bool() const { return ok(); }
};

struct CoreParams {
  /** USB-to-RS485 serial path shared by the Channel and Effect cards. */
  std::string rs485_port;
  /** Channel Card USB CDC path used for VM and attack uploads. */
  std::string channel_cdc_port;
  /** Exact Channel USB audio name, or empty for automatic selection. */
  std::string channel_audio_device;
  /** Exact MIDI input name, or empty to leave MIDI disabled. */
  std::string midi_port;
  /** RS485 baud rate. */
  uint32_t baud = 921600;
  /** Initial Channel output attenuation in decibels. */
  uint8_t attenuation_db = 6;
};

struct MidiPort {
  unsigned index = 0;
  std::string name;
};

enum class MidiPlayback : uint8_t {
  Oscillator = 0,
  Samples,
};

enum class Waveform : uint8_t {
  Sine = 0,
  Pulse,
  Triangle,
  Saw,
};

struct SampleDefinition {
  uint16_t id = 0;
  /** Combined WAV or signed 16-bit little-endian raw recording. */
  std::string sample_file;
  /** Pre-split signed 16-bit or 32-bit little-endian attack. */
  std::string attack_file;
  /** Pre-split signed 16-bit little-endian BODY at 48 kHz. */
  std::string body_file;
  double root_hz = 261.625565;
  /** Sample rate for raw sample_file input; ignored for WAV input. */
  uint32_t raw_sample_rate_hz = 48000;
};

struct VoiceStatus {
  uint8_t active_mask = 0;
  uint8_t pending_mask = 0;
  uint8_t refill_voice = 0xFF;
  uint16_t capacity = 0;
  std::array<uint8_t, 8> session{};
  std::array<uint16_t, 8> fill{};
  std::array<uint16_t, 8> free_samples{};
};

struct EffectStatus {
  bool phantom_enabled = false;
  bool phantom_power_good = false;
  bool audio_enabled = false;
};

/** High-level host interface for the CMI cards, MIDI, VM, and sample playback. */
class Core {
public:
  using ErrorHandler = std::function<void(const Result &)>;

  explicit Core(CoreParams params);
  ~Core();

  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;

  /** Enumerate MIDI inputs whose exact names may be assigned in CoreParams. */
  static std::vector<MidiPort> listMidiPorts();

  /** Open hardware and apply safe defaults. */
  Result connect();
  /** Stop playback and close every owned device. Safe to call repeatedly. */
  Result disconnect();
  bool isConnected() const;
  /** True when hardware, all voice programs, and configured MIDI are ready. */
  bool isReady() const;
  /** Bit N is set when Channel voice N has a program loaded by this Core. */
  uint8_t loadedProgramMask() const;

  /** Receive asynchronous MIDI and streaming errors from the worker thread. */
  void setErrorHandler(ErrorHandler handler);

  /** Compile and load a source file into one Channel voice slot. */
  Result loadVoiceScript(uint8_t voice, const std::string &path);
  /** Compile and load source text into one Channel voice slot. */
  Result loadVoiceScriptSource(uint8_t voice, const std::string &source);

  /** Load one validated sample and configure its root pitch. */
  Result loadSample(const SampleDefinition &sample);
  /** Validate the complete bank, then load it in ascending sample-ID order. */
  Result loadSampleBank(const std::vector<SampleDefinition> &samples);
  /** Load the documented wN attack/BODY folder layout. */
  Result loadSampleFolder(const std::string &path);
  Result setSampleRoot(uint16_t sample_id, double root_hz);

  /** Play the VM oscillator path on a fixed hardware voice. */
  Result noteOn(uint8_t voice, uint8_t midi_key);
  /** Play a loaded attack/BODY sample on a fixed hardware voice. */
  Result sampleNoteOn(uint8_t voice, uint8_t midi_key, uint16_t sample_id);
  Result noteOff(uint8_t voice);
  Result allNotesOff();

  /** Select oscillator or sample playback for automatic MIDI handling. */
  Result setMidiPlayback(MidiPlayback playback);
  Result setMidiSampleMap(const std::array<uint16_t, 128> &sample_ids);

  Result setAttenuation(uint8_t attenuation_db);
  Result setWaveform(Waveform waveform, double parameter = 0.5);
  Result setFilter(uint8_t voice, double cutoff_hz, double q = 1.0);
  Result setAllFilters(double cutoff_hz, double q = 1.0);
  Result setFilterPitchTracking(uint8_t voice, double amount);
  Result setAllFilterPitchTracking(double amount);

  Result queryVoiceStatus(VoiceStatus &status);
  Result recover();

  Result queryEffectStatus(EffectStatus &status);
  Result setPhantomPower(bool enabled);
  Result setEffectAudioEnabled(bool enabled);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cmi
