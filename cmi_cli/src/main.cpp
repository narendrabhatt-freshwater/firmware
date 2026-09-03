#include <cmi/core.hpp>
#include "cardlink/audio/builtin_waves.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

volatile std::sig_atomic_t running = 1;

struct Options {
  fs::path project = fs::current_path();
  fs::path script;
  fs::path samples;
  fs::path wavetables;
  fs::path sample;
  std::string rs485;
  std::string cdc;
  std::string midi = "auto";
  std::string audio;
  uint32_t baud = 921600;
  uint32_t raw_rate = 48000;
  uint16_t sample_id = 0;
  double root_hz = 260.0;
  uint8_t attenuation = 6;
  bool watch = true;
  bool list_midi = false;
  bool help = false;
};

void PrintUsage(const char *program)
{
  std::cout
      << "Usage:\n"
      << "  " << program << " [PROJECT] --rs485 PORT --cdc PORT [options]\n"
      << "  " << program << " --list-midi\n\n"
      << "PROJECT convention:\n"
      << "  channel.be              script uploaded to voices 0-7\n"
      << "  samples/                wN_*.wav/raw/i8 or attack/BODY pairs\n"
      << "  wavetables/             osc0.wav/raw/i8 ... osc7.wav/raw/i8\n\n"
      << "Options:\n"
      << "  --rs485 PORT            USB-to-RS485 serial port\n"
      << "  --cdc PORT              Channel Card USB CDC port\n"
      << "  --script FILE           override PROJECT/channel.be\n"
      << "  --samples DIR           override PROJECT/samples (or '-' to skip)\n"
      << "  --sample FILE           load one fast sample instead of a full bank\n"
      << "  --sample-id N           ID for --sample (default 0)\n"
      << "  --root-hz HZ            root pitch for --sample (default 260)\n"
      << "  --wavetables DIR        override PROJECT/wavetables (or '-' to skip)\n"
      << "  --midi auto|off|NAME    auto-pick by default\n"
      << "  --audio NAME            exact Channel USB audio device\n"
      << "  --attenuation DB        startup attenuation 0..127 (default 6)\n"
      << "  --baud RATE             RS485 baud (default 921600)\n"
      << "  --no-watch              do not reload channel.be after saves\n"
      << "  --list-midi             list MIDI inputs and exit\n"
      << "  --help                  show this help\n";
}

bool Take(int argc, char **argv, int &i, std::string &value)
{
  if (++i >= argc) return false;
  value = argv[i];
  return true;
}

bool ParseUnsigned(const std::string &text, uint32_t max, uint32_t &out)
{
  try {
    size_t used = 0;
    const unsigned long value = std::stoul(text, &used, 10);
    if (used != text.size() || value > max) return false;
    out = static_cast<uint32_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDouble(const std::string &text, double &out)
{
  try {
    size_t used = 0;
    out = std::stod(text, &used);
    return used == text.size();
  } catch (...) {
    return false;
  }
}

bool Parse(int argc, char **argv, Options &o)
{
  bool saw_project = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") { o.help = true; continue; }
    if (arg == "--list-midi") { o.list_midi = true; continue; }
    if (arg == "--no-watch") { o.watch = false; continue; }
    if (!arg.empty() && arg[0] != '-') {
      if (saw_project) {
        std::cerr << "Only one PROJECT directory may be supplied.\n";
        return false;
      }
      o.project = arg;
      saw_project = true;
      continue;
    }
    std::string value;
    if (!Take(argc, argv, i, value)) {
      std::cerr << "Missing value for " << arg << "\n";
      return false;
    }
    uint32_t number = 0;
    if (arg == "--rs485") o.rs485 = value;
    else if (arg == "--cdc") o.cdc = value;
    else if (arg == "--script") o.script = value;
    else if (arg == "--samples") o.samples = value;
    else if (arg == "--sample") o.sample = value;
    else if (arg == "--wavetables") o.wavetables = value;
    else if (arg == "--midi") o.midi = value;
    else if (arg == "--audio") o.audio = value;
    else if (arg == "--sample-id" && ParseUnsigned(value, 247, number)) {
      o.sample_id = static_cast<uint16_t>(number);
    } else if (arg == "--raw-rate" &&
               ParseUnsigned(value, 1000000, number) && number) {
      o.raw_rate = number;
    } else if (arg == "--root-hz" && ParseDouble(value, o.root_hz) &&
               o.root_hz > 0.0) {
      // Parsed directly into the option.
    }
    else if (arg == "--baud" && ParseUnsigned(value, 4000000, number) && number) {
      o.baud = number;
    } else if (arg == "--attenuation" && ParseUnsigned(value, 127, number)) {
      o.attenuation = static_cast<uint8_t>(number);
    } else {
      std::cerr << "Unknown or invalid option: " << arg << " " << value << "\n";
      return false;
    }
  }
  if (o.help || o.list_midi) return true;
  if (o.script.empty()) o.script = o.project / "channel.be";
  if (o.samples.empty()) o.samples = o.project / "samples";
  if (o.wavetables.empty()) o.wavetables = o.project / "wavetables";
  if (o.rs485.empty() || o.cdc.empty()) {
    std::cerr << "--rs485 and --cdc are required (the fw wrapper auto-detects them).\n";
    return false;
  }
  if (!fs::is_regular_file(o.script)) {
    std::cerr << "Script not found: " << o.script << "\n";
    return false;
  }
  return true;
}

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string AutoMidi()
{
  const auto ports = cmi::Core::listMidiPorts();
  if (ports.empty()) return {};
  for (const auto &port : ports) {
    const std::string name = Lower(port.name);
    if (name.find("midi") != std::string::npos &&
        name.find("daw") == std::string::npos) return port.name;
  }
  return ports.front().name;
}

bool Check(const cmi::Result &result, const std::string &operation)
{
  if (result) {
    std::cout << "  ok    " << operation;
    if (!result.message.empty()) std::cout << " — " << result.message;
    std::cout << '\n';
    return true;
  }
  std::cerr << "  error " << operation << ": " << result.message << '\n';
  if (!result.reply.empty()) std::cerr << "        card: " << result.reply << '\n';
  return false;
}

int OscillatorNumber(const fs::path &path)
{
  const std::string name = Lower(path.filename().string());
  if (name.rfind("osc", 0) != 0 || name.size() < 5 ||
      name[3] < '0' || name[3] > '7') return -1;
  const char next = name[4];
  if (next != '.' && next != '_' && next != '-') return -1;
  const std::string ext = Lower(path.extension().string());
  return (ext == ".wav" || ext == ".raw" || ext == ".i8") ? name[3] - '0' : -1;
}

std::map<int, fs::path> FindWavetables(const fs::path &dir)
{
  std::map<int, fs::path> found;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return found;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const int wave = OscillatorNumber(entry.path());
    if (wave >= 0 && found.count(wave) == 0) found[wave] = entry.path();
  }
  return found;
}

void SignalHandler(int) { running = 0; }

bool InputReady()
{
#if defined(_WIN32)
  return WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 200) ==
         WAIT_OBJECT_0;
#else
  pollfd input{STDIN_FILENO, POLLIN, 0};
  const int result = poll(&input, 1, 200);
  return result > 0 && (input.revents & (POLLIN | POLLHUP)) != 0;
#endif
}

bool ReadPath(std::istringstream &input, std::string &path)
{
  input >> std::ws;
  if (input.eof()) return false;
  input >> std::quoted(path);
  return !path.empty();
}

void PrintCommands()
{
  std::cout
      << "Commands:\n"
      << "  status                       show card voice state\n"
      << "  reload [FILE]                compile/upload script to voices 0-7\n"
      << "  samples DIR                  upload an attack/BODY sample bank\n"
      << "  wavetable N FILE             upload logical osc wavetable 0..7\n"
      << "  root ID HZ                   change sample root pitch\n"
      << "  gain DB                      set output attenuation 0..127 dB\n"
      << "  filter VOICE HZ [Q]          set voice filter\n"
      << "  off                          silence every voice\n"
      << "  help                         show these commands\n"
      << "  quit                         silence and exit\n";
}

void RunCommand(cmi::Core &core, const std::string &line,
                const fs::path &default_script)
{
  std::istringstream input(line);
  std::string command;
  input >> command;
  command = Lower(command);
  if (command.empty()) return;
  if (command == "help" || command == "?") {
    PrintCommands();
    return;
  }
  if (command == "quit" || command == "exit" || command == "q") {
    running = 0;
    return;
  }
  if (command == "off" || command == "silence") {
    (void)Check(core.allNotesOff(), "silence");
    return;
  }
  if (command == "status") {
    cmi::VoiceStatus status;
    if (Check(core.queryVoiceStatus(status), "voice status")) {
      std::cout << "        active=0x" << std::hex
                << static_cast<unsigned>(status.active_mask)
                << " pending=0x" << static_cast<unsigned>(status.pending_mask)
                << std::dec << " capacity=" << status.capacity << '\n';
    }
    return;
  }
  if (command == "reload" || command == "script") {
    std::string path;
    if (!ReadPath(input, path)) path = default_script.string();
    (void)Check(core.loadVoiceScriptAll(path), "script voices 0-7");
    return;
  }
  if (command == "samples" || command == "bank") {
    std::string path;
    if (!ReadPath(input, path)) {
      std::cerr << "  usage samples DIR\n";
      return;
    }
    (void)Check(core.loadSampleFolder(path), "sample attack/BODY bank");
    return;
  }
  if (command == "wavetable" || command == "wave" || command == "osc") {
    std::string number_text;
    std::string path;
    uint32_t number = 0;
    input >> number_text;
    if (!ParseUnsigned(number_text, 7, number) || !ReadPath(input, path)) {
      std::cerr << "  usage wavetable N FILE\n";
      return;
    }
    (void)Check(core.loadWavetable(static_cast<uint8_t>(number), path),
                "osc" + std::to_string(number));
    return;
  }
  if (command == "root") {
    std::string id_text;
    std::string hz_text;
    uint32_t id = 0;
    double hz = 0.0;
    input >> id_text >> hz_text;
    if (!ParseUnsigned(id_text, 247, id) || !ParseDouble(hz_text, hz) ||
        !(hz > 0.0)) {
      std::cerr << "  usage root ID HZ\n";
      return;
    }
    (void)Check(core.setSampleRoot(static_cast<uint16_t>(id), hz),
                "sample root");
    return;
  }
  if (command == "gain") {
    std::string db_text;
    uint32_t db = 0;
    input >> db_text;
    if (!ParseUnsigned(db_text, 127, db)) {
      std::cerr << "  usage gain DB\n";
      return;
    }
    (void)Check(core.setAttenuation(static_cast<uint8_t>(db)), "gain");
    return;
  }
  if (command == "filter") {
    std::string voice_text;
    std::string hz_text;
    std::string q_text;
    uint32_t voice = 0;
    double hz = 0.0;
    double q = 1.0;
    input >> voice_text >> hz_text;
    if (input >> q_text) {
      if (!ParseDouble(q_text, q)) q = -1.0;
    }
    if (!ParseUnsigned(voice_text, 7, voice) || !ParseDouble(hz_text, hz) ||
        !(hz > 0.0) || !(q > 0.0)) {
      std::cerr << "  usage filter VOICE HZ [Q]\n";
      return;
    }
    (void)Check(core.setFilter(static_cast<uint8_t>(voice), hz, q), "filter");
    return;
  }
  std::cerr << "  unknown command: " << command << " (type help)\n";
}

} // namespace

int main(int argc, char **argv)
{
  Options options;
  if (!Parse(argc, argv, options)) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (options.help) { PrintUsage(argv[0]); return 0; }
  if (options.list_midi) {
    const auto ports = cmi::Core::listMidiPorts();
    if (ports.empty()) std::cout << "No MIDI inputs found.\n";
    for (const auto &port : ports) std::cout << port.index << ": " << port.name << '\n';
    return 0;
  }

  std::error_code ec;
  options.project = fs::weakly_canonical(options.project, ec);
  options.script = fs::weakly_canonical(options.script, ec);
  if (!options.sample.empty()) options.sample = fs::weakly_canonical(options.sample, ec);
  if (options.samples != "-") options.samples = fs::weakly_canonical(options.samples, ec);
  if (options.wavetables != "-") options.wavetables = fs::weakly_canonical(options.wavetables, ec);

  std::string midi = options.midi;
  if (midi == "auto") {
    midi = AutoMidi();
    if (midi.empty()) {
      std::cerr << "No MIDI input found. Connect one, choose --midi NAME, or use --midi off.\n";
      return 1;
    }
  }
  else if (midi == "off") midi.clear();

  std::cout << "CMI play\n"
            << "  project " << options.project << '\n'
            << "  RS485  " << options.rs485 << '\n'
            << "  CDC    " << options.cdc << '\n'
            << "  MIDI   " << (midi.empty() ? "off" : midi) << '\n'
            << std::flush;

  cmi::CoreParams params;
  params.rs485_port = options.rs485;
  params.channel_cdc_port = options.cdc;
  params.channel_audio_device = options.audio;
  params.midi_port = midi;
  params.baud = options.baud;
  params.attenuation_db = options.attenuation;
  cmi::Core core(params);
  core.setErrorHandler([](const cmi::Result &error) {
    if (error.code == cmi::ErrorCode::Timeout) {
      std::cerr << "  async RS485 timeout: no Channel Card reply";
    } else if (error.code == cmi::ErrorCode::AudioError) {
      std::cerr << "  async BODY audio error";
    } else {
      std::cerr << "  async playback error";
    }
    if (!error.message.empty() && error.message != "(empty)") {
      std::cerr << ": " << error.message;
    }
    std::cerr << '\n';
  });

  if (!Check(core.connect(), "connect")) return 1;

  if (!options.sample.empty()) {
    cmi::SampleDefinition sample;
    sample.id = options.sample_id;
    sample.sample_file = options.sample.string();
    sample.root_hz = options.root_hz;
    sample.raw_sample_rate_hz = options.raw_rate;
    if (!Check(core.loadSample(sample), "quick sample attack/BODY")) return 1;
    std::array<uint16_t, 128> midi_map;
    midi_map.fill(options.sample_id);
    if (!Check(core.setMidiSampleMap(midi_map), "MIDI sample map")) return 1;
  } else if (options.samples != "-" && fs::is_directory(options.samples)) {
    if (!Check(core.loadSampleFolder(options.samples), "sample attack/BODY bank")) return 1;
  } else if (options.samples != "-") {
    std::cerr << "  error sample directory not found: " << options.samples
              << " (use --samples - only for intentional no-sample setup)\n";
    return 1;
  } else {
    std::cout << "  skip  sample bank (explicitly disabled)\n";
  }

  if (options.wavetables != "-") {
    const auto waves = FindWavetables(options.wavetables);
    for (int number = 0; number < 8; ++number) {
      const auto custom = waves.find(number);
      if (custom != waves.end()) {
        if (!Check(core.loadWavetable(static_cast<uint8_t>(number),
                                     custom->second.string()),
                   "osc" + std::to_string(number) + " " +
                       custom->second.filename().string())) {
          return 1;
        }
      } else {
        if (!Check(core.loadWavetable(static_cast<uint8_t>(number),
                                     cardlink::audio::MakeBuiltinWavetable(
                                         static_cast<uint8_t>(number))),
                   "osc" + std::to_string(number) + " built-in " +
                       cardlink::audio::BuiltinWavetableName(
                           static_cast<uint8_t>(number)))) {
          return 1;
        }
      }
    }
  }

  if (!Check(core.loadVoiceScriptAll(options.script.string()), "script voices 0-7")) return 1;

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  std::cout << "\nReady — "
            << (midi.empty() ? "session is running" : "MIDI is live")
            << ". ";
  if (options.watch) std::cout << "Save " << options.script.filename() << " to reload; ";
  std::cout << "type help for commands.\n";

  auto stamp = fs::last_write_time(options.script, ec);
  while (running != 0) {
    std::cout << "cmi> " << std::flush;
    while (running != 0) {
      if (options.watch) {
        const auto next = fs::last_write_time(options.script, ec);
        if (!ec && next != stamp) {
          stamp = next;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          std::cout << "\nScript saved — reloading...\n";
          (void)Check(core.loadVoiceScriptAll(options.script.string()),
                      "script voices 0-7");
          std::cout << "cmi> " << std::flush;
        }
      }
      if (!InputReady()) continue;
      std::string line;
      if (!std::getline(std::cin, line)) running = 0;
      else RunCommand(core, line, options.script);
      break;
    }
  }

  std::cout << "\nStopping...\n";
  (void)core.allNotesOff();
  return Check(core.disconnect(), "disconnect") ? 0 : 1;
}
