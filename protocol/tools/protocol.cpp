#include "cardlink/audio/sample_bulk.hpp"
#include "cardlink/rs485/controller.hpp"
#include "cardlink/sample/client.hpp"
#include "cardlink/sequence/crash_sequence.hpp"
#include "cardlink/serial_port.hpp"
#include "cardlink/usb/cdc_port.hpp"
#include "cardlink/vm/compiler.hpp"
#include "cardlink/vm/uploader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

class NoteGateBarrier
{
public:
  void Begin()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_ == 0u) {
      okay_ = true;
    }
    ++pending_;
  }

  void Complete(bool okay)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      okay_ = okay_ && okay;
      if (pending_ > 0u) {
        --pending_;
      }
    }
    ready_.notify_all();
  }

  bool Wait(std::chrono::milliseconds timeout = 2s)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_.wait_for(lock, timeout, [this] { return pending_ == 0u; }) &&
           okay_;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  size_t pending_ = 0u;
  bool okay_ = true;
};

void Usage()
{
  std::fprintf(stderr,
      "usage: protocol --crash FILE [--g DB] [--r N]\n"
      "                [--s FILE | --d DIR] [--dry-run]\n"
      "       protocol --script PROGRAM.be [--u CDC] [--dry-run]\n"
      "       overrides: --p RS485 --u CDC --b BAUD\n");
}
bool SleepMs(uint32_t ms)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(ms);
  while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::this_thread::sleep_for(
        std::min(left, std::chrono::milliseconds(10)));
  }
  return !g_stop.load();
}

bool WaitQueue(cardlink::rs485::Controller &bus,
               std::chrono::milliseconds timeout = 2s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
    if (bus.QueueDepth() == 0u) {
      std::this_thread::sleep_for(20ms);
      if (bus.QueueDepth() == 0u) {
        return !bus.BusFault();
      }
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

bool IsChannelCdc(const std::string &path)
{
  return path.find("usbmodemCHCARD") != std::string::npos ||
         path.find("ttyACM") != std::string::npos;
}

std::string PickCdc(const std::vector<std::string> &ports)
{
  for (const auto &path : ports) {
    if (path.find("usbmodemCHCARD") != std::string::npos) {
      return path;
    }
  }
  for (const auto &path : ports) {
    if (IsChannelCdc(path) &&
        !cardlink::usb::LooksLikeRs485AdapterPath(path)) {
      return path;
    }
  }
  return {};
}

std::vector<std::string> Rs485Candidates(
    const std::vector<std::string> &ports, const std::string &cdc)
{
  std::vector<std::string> preferred;
  std::vector<std::string> fallback;
  for (const auto &path : ports) {
    if (path == cdc || IsChannelCdc(path)) {
      continue;
    }
    (cardlink::usb::LooksLikeRs485AdapterPath(path) ? preferred : fallback)
        .push_back(path);
  }
  preferred.insert(preferred.end(), fallback.begin(), fallback.end());
  return preferred;
}

bool DirHasRawBank(const fs::path &dir)
{
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return false;
  }
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.size() >= 6 && name[0] == 'w' &&
        name.compare(name.size() - 4, 4, ".raw") == 0) {
      return true;
    }
  }
  return false;
}

std::string FindWavesNear(fs::path start)
{
  for (int depth = 0; depth < 10 && !start.empty(); ++depth) {
    const fs::path candidates[] = {
        start / "waves", start / "cmi_control" / "waves"};
    for (const auto &candidate : candidates) {
      if (DirHasRawBank(candidate)) {
        return candidate.string();
      }
    }
    const fs::path parent = start.parent_path();
    if (parent == start) {
      break;
    }
    start = parent;
  }
  return {};
}

std::string ExecutableDir()
{
#if defined(__APPLE__)
  char path[4096];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    std::error_code ec;
    return fs::weakly_canonical(fs::path(path), ec).parent_path().string();
  }
#elif defined(__linux__)
  std::error_code ec;
  const fs::path path = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    return path.parent_path().string();
  }
#endif
  return {};
}

std::string FindWavesDir()
{
  std::string found = FindWavesNear(fs::current_path());
  if (!found.empty()) {
    return found;
  }
  const std::string executable = ExecutableDir();
  return executable.empty() ? std::string{} : FindWavesNear(executable);
}

std::string FindWave(const fs::path &dir, uint16_t wave_id)
{
  const std::string prefix = "w" + std::to_string(wave_id) + "_";
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.rfind(prefix, 0) == 0 &&
        name.size() >= 4 && name.compare(name.size() - 4, 4, ".raw") == 0) {
      return entry.path().string();
    }
  }
  return {};
}

std::set<uint16_t> ReferencedWaves(
    const cardlink::sequence::CrashSequence &sequence)
{
  std::set<uint16_t> waves;
  for (const auto &step : sequence.steps) {
    if (step.kind == cardlink::sequence::CrashStepKind::Note) {
      waves.insert(step.wave_id);
    }
  }
  return waves;
}

bool LoadSamples(cardlink::sample::Client &samples,
                 const cardlink::sequence::CrashSequence &sequence,
                 const std::string &dir, const std::string &sample_file,
                 std::string &error)
{
  const fs::path roots = fs::path(dir) / "roots.txt";
  if (sample_file.empty() && fs::exists(roots) &&
      !samples.Mixer().LoadRootsFile(roots.string(), error)) {
    return false;
  }
  if (!samples.BeginCdc(error)) {
    return false;
  }
  struct CdcGuard {
    cardlink::sample::Client &client;
    ~CdcGuard() { client.EndCdc(); }
  } guard{samples};
  (void)guard;

  for (const uint16_t wave_id : ReferencedWaves(sequence)) {
    const std::string path = sample_file.empty() ? FindWave(dir, wave_id)
                                                  : sample_file;
    if (path.empty()) {
      error = "missing w" + std::to_string(wave_id) + "_*.raw in " + dir;
      return false;
    }
    std::printf("uploading w%u from %s\n", static_cast<unsigned>(wave_id),
                path.c_str());
    if (!samples.LoadWave(wave_id, path, error)) {
      return false;
    }
    const double root_hz = samples.Mixer().BodyRootHz(wave_id);
    if (root_hz > 0.0 && !samples.SetRootHz(wave_id, root_hz, error)) {
      return false;
    }
  }
  return true;
}

bool QueueCommand(cardlink::rs485::Controller &bus,
                  const std::string &command)
{
  auto result = std::make_shared<std::promise<bool>>();
  auto ready = result->get_future();
  if (bus.QueueChannel(
          [command, result](cardproto::ChannelClient &channel) {
            auto reply = channel.Exec(command);
            result->set_value(reply.ok());
            return reply;
          }) != cardlink::rs485::QueueResult::Ok) {
    return false;
  }
  return ready.wait_for(2s) == std::future_status::ready && ready.get();
}

} // namespace

int main(int argc, char **argv)
{
  std::string crash_path;
  std::string vm_script_path;
  std::string port_path;
  std::string cdc_path;
  std::string samples_dir;
  std::string sample_file;
  uint32_t baud = 921600;
  uint32_t atten = 6;
  constexpr uint32_t slot = 0;
  uint32_t repeat_override = 0;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    auto need = [&](const char *flag) -> const char * {
      if (++i >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag);
        Usage();
        std::exit(2);
      }
      return argv[i];
    };
    if (std::strcmp(arg, "--crash") == 0 || std::strcmp(arg, "--c") == 0) {
      crash_path = need(arg);
    } else if (std::strcmp(arg, "--script") == 0 ||
               std::strcmp(arg, "--vm-script") == 0 ||
               std::strcmp(arg, "--vm") == 0) {
      vm_script_path = need(arg);
    } else if (std::strcmp(arg, "--port") == 0 ||
               std::strcmp(arg, "--p") == 0) {
      port_path = need(arg);
    } else if (std::strcmp(arg, "--cdc") == 0 ||
               std::strcmp(arg, "--u") == 0) {
      cdc_path = need(arg);
    } else if (std::strcmp(arg, "--samples") == 0 ||
               std::strcmp(arg, "--d") == 0) {
      samples_dir = need(arg);
    } else if (std::strcmp(arg, "--sample") == 0 ||
               std::strcmp(arg, "--s") == 0) {
      sample_file = need(arg);
    } else if (std::strcmp(arg, "--baud") == 0 ||
               std::strcmp(arg, "--b") == 0) {
      baud = static_cast<uint32_t>(std::strtoul(need(arg), nullptr, 10));
    } else if (std::strcmp(arg, "--atten") == 0 ||
               std::strcmp(arg, "--g") == 0) {
      atten = static_cast<uint32_t>(std::strtoul(need(arg), nullptr, 10));
    } else if (std::strcmp(arg, "--repeat") == 0 ||
               std::strcmp(arg, "--r") == 0) {
      repeat_override =
          static_cast<uint32_t>(std::strtoul(need(arg), nullptr, 10));
    } else if (std::strcmp(arg, "--dry-run") == 0) {
      dry_run = true;
    } else if (std::strcmp(arg, "-h") == 0 ||
               std::strcmp(arg, "--help") == 0) {
      Usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg);
      Usage();
      return 2;
    }
  }
  if ((crash_path.empty() == vm_script_path.empty()) || baud == 0 ||
      atten > 127 ||
      (!sample_file.empty() && !samples_dir.empty())) {
    Usage();
    return 2;
  }

  if (!vm_script_path.empty()) {
    if (!fs::is_regular_file(vm_script_path)) {
      std::fprintf(stderr, "Berry script not found: %s\n",
                   vm_script_path.c_str());
      return 2;
    }
    cardlink::vm::BerryCompiler compiler;
    const auto compiled = compiler.CompileChannelFile(vm_script_path);
    if (!compiled.ok) {
      std::fprintf(stderr, "%s: %s\n", vm_script_path.c_str(),
                   compiled.message.c_str());
      return 2;
    }
    std::printf("compiled %s -> %zu-byte FWSC\n", vm_script_path.c_str(),
                compiled.program.size());
    if (dry_run) {
      std::printf("dry run; no card opened\n");
      return 0;
    }
    const auto ports = cardlink::SerialPort::ListPorts();
    if (cdc_path.empty()) cdc_path = PickCdc(ports);
    if (cdc_path.empty()) {
      std::fprintf(stderr,
                   "Channel Card CDC not found (expected CHCARD/ttyACM)\n");
      return 1;
    }
    cardlink::SerialPort cdc;
    std::string cdc_error;
    if (!cardlink::usb::OpenCdcPort(cdc, cdc_path, cdc_error)) {
      std::fprintf(stderr, "%s\n", cdc_error.c_str());
      return 1;
    }
    std::printf("selected CDC %s\n", cdc_path.c_str());
    cardlink::vm::VmUploader uploader(cdc);
    int last_voice = -1;
    const auto uploaded = uploader.UploadAll(
        compiled.program.data(), compiled.program.size(),
        [&last_voice](uint8_t voice, float) {
          if (last_voice != static_cast<int>(voice)) {
            last_voice = static_cast<int>(voice);
            std::printf("uploading voice %u\n", static_cast<unsigned>(voice));
          }
        });
    cdc.Close();
    if (!uploaded.ok) {
      std::fprintf(stderr, "%s\n", uploaded.message.c_str());
      return 1;
    }
    std::printf("ok: uploaded %s to voices 0-7\n", vm_script_path.c_str());
    return 0;
  }

  cardlink::sequence::CrashSequence sequence;
  std::string error;
  if (!cardlink::sequence::LoadCrashSequence(crash_path, sequence, error)) {
    std::fprintf(stderr, "%s: %s\n", crash_path.c_str(), error.c_str());
    return 2;
  }
  if (dry_run) {
    std::printf("dry run; voice slot %u\n", slot);
    for (const auto &step : sequence.steps) {
      using cardlink::sequence::CrashStepKind;
      if (step.kind == CrashStepKind::Note) {
        std::printf("  line %u: %-4s key %u, wave w%u, %ums", step.line,
                    step.label.c_str(), static_cast<unsigned>(step.key),
                    static_cast<unsigned>(step.wave_id), step.duration_ms);
        if (step.release) {
          std::printf(" then %ums voice steal", step.release_ms);
        } else {
          std::printf(" then note-off");
        }
        std::printf("\n");
      } else if (step.kind == CrashStepKind::Delay) {
        std::printf("  line %u: delay %ums\n", step.line, step.duration_ms);
      } else if (step.kind == CrashStepKind::Command) {
        std::printf("  line %u: %s\n", step.line, step.command.c_str());
      } else {
        std::printf("  line %u: crash release %ums\n", step.line,
                    static_cast<unsigned>(step.release_ms));
      }
    }
    return 0;
  }

  const auto ports = cardlink::SerialPort::ListPorts();
  if (cdc_path.empty()) {
    cdc_path = PickCdc(ports);
  }
  if (cdc_path.empty()) {
    std::fprintf(stderr, "Channel Card CDC not found (expected CHCARD/ttyACM)\n");
    return 1;
  }
  if (!sample_file.empty() && !fs::is_regular_file(sample_file)) {
    std::fprintf(stderr, "sample file not found: %s\n", sample_file.c_str());
    return 1;
  }
  if (sample_file.empty() && samples_dir.empty()) {
    samples_dir = FindWavesDir();
  }
  if (sample_file.empty() &&
      (samples_dir.empty() || !DirHasRawBank(samples_dir))) {
    std::fprintf(stderr,
                 "sample bank not found; use --samples DIR containing wN_*.raw\n");
    return 1;
  }

  cardlink::rs485::Controller bus;
  bus.SetLogHandler([](const std::string &message) {
    if (message.rfind("warn:", 0) == 0 ||
        message.rfind("err:", 0) == 0 ||
        message.find(" err:") != std::string::npos) {
      std::printf("%s\n", message.c_str());
    }
  });
  bus.SetPollLogHandler([](const std::string &message) {
    if (message.rfind("warn:", 0) == 0 ||
        message.rfind("err:", 0) == 0 ||
        message.find(" err:") != std::string::npos) {
      std::printf("%s\n", message.c_str());
    }
  });
  if (!port_path.empty()) {
    if (!bus.Open(port_path, baud, atten)) {
      std::fprintf(stderr, "cannot open Channel Card RS485 at %s\n",
                   port_path.c_str());
      return 1;
    }
  } else {
    for (const auto &candidate : Rs485Candidates(ports, cdc_path)) {
      std::printf("probing RS485 %s\n", candidate.c_str());
      if (bus.Open(candidate, baud, atten)) {
        port_path = candidate;
        break;
      }
    }
    if (port_path.empty()) {
      std::fprintf(stderr, "Channel Card RS485 adapter not found\n");
      return 1;
    }
  }
  std::printf("selected RS485 %s\nselected CDC   %s\n%s          %s\n",
              port_path.c_str(), cdc_path.c_str(),
              sample_file.empty() ? "sample bank" : "sample file",
              sample_file.empty() ? samples_dir.c_str() : sample_file.c_str());

  cardlink::sample::Client samples;
  NoteGateBarrier note_gate;
  samples.SetCdcPath(cdc_path);
  samples.SetConsole([&bus](const std::string &command) {
    (void)bus.QueueExec(cardproto::Target::Channel, command);
  });
  samples.SetNoteGate(
      [&bus, &note_gate](const cardlink::sample::NoteRequest &note,
                        cardlink::sample::Client::NoteGateStart start,
                        cardlink::sample::Client::NoteGateDone done) {
        note_gate.Begin();
        const auto queued = bus.QueueChannel(
            [note, start = std::move(start), done = std::move(done),
             &bus, &note_gate](cardproto::ChannelClient &channel) {
              start();
              if (!note.note_on) {
                auto result = channel.NoteOff(note.voice);
                if (result.ok()) bus.AcknowledgeSlotOff(note.voice);
                done(result.ok());
                note_gate.Complete(result.ok());
                return result;
              }
              const std::string assign =
                  "aw " + std::to_string(static_cast<unsigned>(note.voice)) +
                  " " + std::to_string(static_cast<unsigned>(note.wave_id));
              auto result = channel.Exec(assign);
              if (result.ok()) {
                result = channel.StreamNoteOn(note.voice, note.key, note.session);
              }
              if (result.ok()) bus.AcknowledgeSlotKey(note.voice, note.key);
              done(result.ok());
              note_gate.Complete(result.ok());
              return result;
            });
        if (queued != cardlink::rs485::QueueResult::Ok) {
          note_gate.Complete(false);
        }
        return queued == cardlink::rs485::QueueResult::Ok;
      });
  bus.SetIdleHandler([&samples](uint8_t voice) { samples.Silence(voice); });

  if (!LoadSamples(samples, sequence, samples_dir, sample_file, error)) {
    std::fprintf(stderr, "sample load/upload failed: %s\n", error.c_str());
    bus.Close();
    return 1;
  }
  if (!WaitQueue(bus, 5s)) {
    std::fprintf(stderr, "RS485 queue stalled after sample upload\n");
    bus.Close();
    return 1;
  }

  cardlink::audio::SampleBulkOut bulk;
  bulk.BindMixer(samples.Mixer());
  bus.SetVqHandler(
      [&bulk](const cardproto::VoiceQuery &status) {
        bulk.SubmitStatus(status);
      });
  if (!bulk.Start(error)) {
    std::fprintf(stderr, "Channel Card BODY output failed: %s\n", error.c_str());
    bus.Close();
    return 1;
  }
  std::printf("samples uploaded; BODY stream started\n");

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  /* Leading configuration is a one-time preamble, so it does not add hidden
   * RS485 latency between the final delay and the next repeated note. */
  size_t first_play_step = 0;
  while (first_play_step < sequence.steps.size()) {
    const auto &step = sequence.steps[first_play_step];
    if (step.kind != cardlink::sequence::CrashStepKind::CrashRelease) {
      break;
    }
    ++first_play_step;
  }

  uint64_t pass = 0;
  bool okay = true;
  const uint64_t passes = repeat_override > 0u
                              ? repeat_override
                              : sequence.repeat_count;
  while (!g_stop.load() &&
         (sequence.repeat_forever && repeat_override == 0u
              ? true
              : pass < passes)) {
    ++pass;
    std::printf("pass %llu\n", static_cast<unsigned long long>(pass));
    for (size_t step_index = first_play_step;
         step_index < sequence.steps.size(); ++step_index) {
      const auto &step = sequence.steps[step_index];
      if (g_stop.load()) {
        break;
      }
      using cardlink::sequence::CrashStepKind;
      if (step.kind == CrashStepKind::Note) {
        std::printf("  %-4s w%u for %ums", step.label.c_str(),
                    static_cast<unsigned>(step.wave_id), step.duration_ms);
        if (step.release) {
          std::printf(" then %ums voice steal", step.release_ms);
        } else {
          std::printf(" then note-off");
        }
        std::printf("\n");
        samples.NoteOn(static_cast<uint8_t>(slot), step.key, step.wave_id);
        if (!note_gate.Wait() || !SleepMs(step.duration_ms)) {
          okay = g_stop.load();
          break;
        }
        if (!step.release) {
          samples.NoteOff(static_cast<uint8_t>(slot));
          if (!note_gate.Wait()) {
            okay = g_stop.load();
            break;
          }
        }
      } else if (step.kind == CrashStepKind::Delay) {
        if (!SleepMs(step.duration_ms)) {
          break;
        }
      } else if (step.kind == CrashStepKind::CrashRelease) {
        std::fprintf(stderr, "line %u: crash timing belongs to ABI6 Berry\n",
                     step.line);
        okay = false;
        break;
      } else if (!QueueCommand(bus, step.command)) {
        okay = false;
        break;
      }
    }
    if (!okay) {
      break;
    }
  }

  bus.RequestSilence();
  (void)WaitQueue(bus);
  bulk.Stop();
  bus.Close();
  if (g_stop.load()) {
    std::printf("stopped; all notes off\n");
  }
  return okay ? 0 : 1;
}
