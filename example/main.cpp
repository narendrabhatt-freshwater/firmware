#include <cmi/core.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
  std::string rs485_port;
  std::string cdc_port;
  std::string midi_port;
  std::string audio_device;
  std::vector<std::string> scripts;
  std::string sample_file;
  uint32_t raw_rate = 48000;
  uint32_t duration_ms = 2000;
  uint16_t sample_id = 60;
  uint8_t voice = 0;
  uint8_t key = 60;
  double root_hz = 261.625565;
  bool list_midi = false;
  bool help = false;
};

void PrintUsage(const char *program)
{
  std::cout
      << "Usage:\n"
      << "  " << program << " --list-midi\n"
      << "  " << program
      << " --rs485 PORT --cdc PORT --script FILE [options]\n\n"
      << "Required:\n"
      << "  --rs485 PORT       USB-to-RS485 serial path\n"
      << "  --cdc PORT         Channel Card USB CDC path\n"
      << "  --script FILE      Repeat once for all voices or exactly eight times\n\n"
      << "Playback options:\n"
      << "  --voice N          Hardware voice 0..7 (default 0)\n"
      << "  --key N            MIDI key 0..127 (default 60)\n"
      << "  --duration-ms N    Playback time 1..600000 (default 2000)\n"
      << "  --sample FILE      Optional combined WAV or signed 16-bit raw file\n"
      << "  --sample-id N      Sample ID 0..255 (default 60)\n"
      << "  --root-hz HZ       Sample root frequency (default 261.625565)\n"
      << "  --raw-rate HZ      Raw-file sample rate (default 48000)\n"
      << "  --midi NAME        Exact MIDI input name\n"
      << "  --audio NAME       Exact Channel USB audio-device name\n"
      << "  --help             Show this message\n";
}

bool ParseUnsigned(const std::string &text, uint32_t maximum, uint32_t &value)
{
  if (text.empty()) return false;
  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || parsed > maximum) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool ParsePositiveDouble(const std::string &text, double &value)
{
  if (text.empty()) return false;
  char *end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      !std::isfinite(parsed) || !(parsed > 0.0)) {
    return false;
  }
  value = parsed;
  return true;
}

bool TakeValue(int argc, char **argv, int &index, std::string &value)
{
  if (index + 1 >= argc) return false;
  value = argv[++index];
  return true;
}

bool ParseOptions(int argc, char **argv, Options &options)
{
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    std::string value;
    uint32_t number = 0;

    if (argument == "--help") {
      options.help = true;
      continue;
    }
    if (argument == "--list-midi") {
      options.list_midi = true;
      continue;
    }
    const bool takes_value =
        argument == "--rs485" || argument == "--cdc" ||
        argument == "--midi" || argument == "--audio" ||
        argument == "--script" || argument == "--sample" ||
        argument == "--root-hz" || argument == "--voice" ||
        argument == "--key" || argument == "--sample-id" ||
        argument == "--raw-rate" || argument == "--duration-ms";
    if (!takes_value) {
      std::cerr << "Unknown option: " << argument << '\n';
      return false;
    }
    if (!TakeValue(argc, argv, index, value)) {
      std::cerr << "Missing value for " << argument << '\n';
      return false;
    }

    if (argument == "--rs485") options.rs485_port = value;
    else if (argument == "--cdc") options.cdc_port = value;
    else if (argument == "--midi") options.midi_port = value;
    else if (argument == "--audio") options.audio_device = value;
    else if (argument == "--script") options.scripts.push_back(value);
    else if (argument == "--sample") options.sample_file = value;
    else if (argument == "--root-hz") {
      if (!ParsePositiveDouble(value, options.root_hz)) {
        std::cerr << "Invalid root frequency: " << value << '\n';
        return false;
      }
    } else if (argument == "--voice") {
      if (!ParseUnsigned(value, 7, number)) {
        std::cerr << "Invalid voice: " << value << '\n';
        return false;
      }
      options.voice = static_cast<uint8_t>(number);
    } else if (argument == "--key") {
      if (!ParseUnsigned(value, 127, number)) {
        std::cerr << "Invalid MIDI key: " << value << '\n';
        return false;
      }
      options.key = static_cast<uint8_t>(number);
    } else if (argument == "--sample-id") {
      if (!ParseUnsigned(value, 255, number)) {
        std::cerr << "Invalid sample ID: " << value << '\n';
        return false;
      }
      options.sample_id = static_cast<uint16_t>(number);
    } else if (argument == "--raw-rate") {
      if (!ParseUnsigned(value, std::numeric_limits<uint32_t>::max(), number) ||
          number == 0) {
        std::cerr << "Invalid raw sample rate: " << value << '\n';
        return false;
      }
      options.raw_rate = number;
    } else if (argument == "--duration-ms") {
      if (!ParseUnsigned(value, 600000, number) || number == 0) {
        std::cerr << "Invalid duration: " << value << '\n';
        return false;
      }
      options.duration_ms = number;
    }
  }

  if (options.help || options.list_midi) return true;
  if (options.rs485_port.empty() || options.cdc_port.empty()) {
    std::cerr << "--rs485 and --cdc are required\n";
    return false;
  }
  if (options.scripts.size() != 1 && options.scripts.size() != 8) {
    std::cerr << "Provide --script once or exactly eight times\n";
    return false;
  }
  return true;
}

bool Check(const cmi::Result &result, const char *operation)
{
  if (result) return true;
  std::cerr << operation << " failed: " << result.message << '\n';
  if (!result.reply.empty()) std::cerr << "Card reply: " << result.reply << '\n';
  return false;
}

} // namespace

int main(int argc, char **argv)
{
  Options options;
  if (!ParseOptions(argc, argv, options)) {
    PrintUsage(argv[0]);
    return 2;
  }

  if (options.help) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (options.list_midi) {
    const std::vector<cmi::MidiPort> ports = cmi::Core::listMidiPorts();
    if (ports.empty()) {
      std::cout << "No MIDI input ports found.\n";
      return 0;
    }
    for (const cmi::MidiPort &port : ports) {
      std::cout << port.index << ": " << port.name << '\n';
    }
    return 0;
  }

  cmi::CoreParams params;
  params.rs485_port = options.rs485_port;
  params.channel_cdc_port = options.cdc_port;
  params.midi_port = options.midi_port;
  params.channel_audio_device = options.audio_device;

  cmi::Core core(params);
  core.setErrorHandler([](const cmi::Result &error) {
    std::cerr << "Asynchronous error: " << error.message << '\n';
  });

  if (!Check(core.connect(), "connect")) return 1;
  for (uint8_t voice = 0; voice < 8; ++voice) {
    const std::string &script =
        options.scripts.size() == 1 ? options.scripts.front()
                                    : options.scripts[voice];
    if (!Check(core.loadVoiceScript(voice, script), "loadVoiceScript")) {
      return 1;
    }
  }

  cmi::Result started;
  if (options.sample_file.empty()) {
    started = core.noteOn(options.voice, options.key);
  } else {
    cmi::SampleDefinition sample;
    sample.id = options.sample_id;
    sample.sample_file = options.sample_file;
    sample.root_hz = options.root_hz;
    sample.raw_sample_rate_hz = options.raw_rate;
    if (!Check(core.loadSample(sample), "loadSample")) return 1;

    std::array<uint16_t, 128> midi_map;
    midi_map.fill(options.sample_id);
    if (!Check(core.setMidiSampleMap(midi_map), "setMidiSampleMap") ||
        !Check(core.setMidiPlayback(cmi::MidiPlayback::Samples),
               "setMidiPlayback")) {
      return 1;
    }
    started = core.sampleNoteOn(options.voice, options.key, options.sample_id);
  }

  if (!Check(started, "noteOn")) return 1;
  std::cout << "Playing voice " << static_cast<unsigned>(options.voice)
            << ", MIDI key " << static_cast<unsigned>(options.key) << '\n';
  std::this_thread::sleep_for(std::chrono::milliseconds(options.duration_ms));
  if (!Check(core.noteOff(options.voice), "noteOff")) return 1;
  return Check(core.disconnect(), "disconnect") ? 0 : 1;
}
