#include "cardlink/sample/client.hpp"

#include "cardlink/audio/wave_loader.hpp"
#include "cardlink/serial_port.hpp"
#include "cardlink/usb/attack_upload.hpp"
#include "cardlink/usb/cdc_port.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace cardlink {
namespace sample {
namespace {

bool HasExtI(const std::string &path, const char *ext)
{
  const size_t n = std::strlen(ext);
  if (path.size() < n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a =
        static_cast<unsigned char>(path[path.size() - n + i]);
    const unsigned char b = static_cast<unsigned char>(ext[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

bool IsWaveFile(const std::string &path)
{
  return HasExtI(path, ".wav") || HasExtI(path, ".raw");
}

std::string Basename(const std::string &path)
{
  if (path.empty()) {
    return {};
  }
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool FindNamedPair(const fs::path &dir, int id, std::string &head,
                   std::string &body)
{
  head.clear();
  body.clear();
  char prefix[16];
  std::snprintf(prefix, sizeof prefix, "w%d_", id);
  try {
    if (!fs::exists(dir)) {
      return false;
    }
    for (const auto &ent : fs::directory_iterator(dir)) {
      if (!ent.is_regular_file()) {
        continue;
      }
      const auto name = ent.path().filename().string();
      if (name.rfind(prefix, 0) != 0) {
        continue;
      }
      if (name.find("_head.i32") != std::string::npos) {
        head = ent.path().string();
      } else if (name.find("_body.i16") != std::string::npos) {
        body = ent.path().string();
      }
    }
  } catch (...) {
    return false;
  }
  return !head.empty() && !body.empty();
}

bool FindStemPairs(const fs::path &dir,
                   std::array<std::string, cardlink::audio::kSampleVoices> &heads,
                   std::array<std::string, cardlink::audio::kSampleVoices> &bodies)
{
  heads.fill({});
  bodies.fill({});
  std::vector<fs::path> head_files;
  try {
    for (const auto &ent : fs::directory_iterator(dir)) {
      if (!ent.is_regular_file()) {
        continue;
      }
      const auto name = ent.path().filename().string();
      if (name.size() > 9 &&
          name.compare(name.size() - 9, 9, "_head.i32") == 0) {
        head_files.push_back(ent.path());
      }
    }
  } catch (...) {
    return false;
  }
  std::sort(head_files.begin(), head_files.end());
  int n = 0;
  for (const auto &hp : head_files) {
    if (n >= static_cast<int>(cardlink::audio::kSampleVoices)) {
      break;
    }
    const std::string name = hp.filename().string();
    const std::string stem = name.substr(0, name.size() - 9);
    const fs::path bp = hp.parent_path() / (stem + "_body.i16");
    if (!fs::exists(bp)) {
      continue;
    }
    heads[static_cast<size_t>(n)] = hp.string();
    bodies[static_cast<size_t>(n)] = bp.string();
    ++n;
  }
  return n > 0;
}

} // namespace

void Client::SetConsole(ConsoleFn fn)
{
  console_ = std::move(fn);
}

void Client::SetCdcPath(const std::string &path)
{
  cdc_path_ = path;
}

void Client::Render(int16_t *interleaved, unsigned nframes)
{
  mixer_.Render(interleaved, nframes);
}

void Client::SendConsole(const std::string &cmd)
{
  if (console_) {
    console_(cmd);
  }
}

bool Client::SendCdcLine(const std::string &line, std::string &err)
{
  if (cdc_path_.empty()) {
    err = "err: CDC path not set";
    return false;
  }
  cardlink::SerialPort port;
  if (!cardlink::usb::OpenCdcPort(port, cdc_path_, err)) {
    return false;
  }
  if (!port.Write(reinterpret_cast<const uint8_t *>(line.data()),
                  line.size())) {
    err = "err: CDC write";
    return false;
  }
  port.DrainOutput();
  uint8_t buf[128];
  (void)port.ReadTimeout(buf, sizeof(buf), 200);
  return true;
}

bool Client::UploadAttack(uint8_t voice, const int32_t *q31, size_t nsamp,
                          std::string &err)
{
  if (voice >= cardlink::audio::kSampleVoices || q31 == nullptr ||
      nsamp == 0 || nsamp > cardlink::audio::kAttackSamples) {
    err = "err: attack must be 1..8192 Q31 samples";
    return false;
  }
  if (cdc_path_.empty()) {
    err = "err: CDC path not set";
    return false;
  }
  cardlink::SerialPort port;
  if (!cardlink::usb::OpenCdcPort(port, cdc_path_, err)) {
    return false;
  }
  cardlink::usb::AttackUploader up(port);
  auto r = up.Upload(voice, reinterpret_cast<const uint8_t *>(q31),
                     nsamp * sizeof(int32_t));
  if (!r.ok) {
    err = r.message;
    return false;
  }
  mixer_.SetAttackLen(voice, static_cast<unsigned>(nsamp));
  return true;
}

bool Client::UploadAttackFile(uint8_t voice, const std::string &path,
                              std::string &err)
{
  if (cdc_path_.empty()) {
    err = "err: CDC path not set";
    return false;
  }
  cardlink::SerialPort port;
  if (!cardlink::usb::OpenCdcPort(port, cdc_path_, err)) {
    return false;
  }
  cardlink::usb::AttackUploader up(port);
  auto r = up.UploadFile(voice, path);
  if (!r.ok) {
    err = r.message;
    return false;
  }
  std::error_code ec;
  const auto bytes = fs::file_size(path, ec);
  if (!ec && bytes >= 4u && (bytes % 4u) == 0u) {
    mixer_.SetAttackLen(voice, static_cast<unsigned>(bytes / 4u));
  }
  return true;
}

void Client::SetLabel(uint8_t voice)
{
  auto &slot = slots_[voice];
  std::string base = Basename(slot.head_path);
  if (base.empty()) {
    base = Basename(slot.body_path);
  }
  if (base.size() > 3 && base[0] == 'w' && base[1] >= '0' && base[1] <= '7' &&
      base[2] == '_') {
    base = base.substr(3);
  }
  for (const char *suf :
       {"_head.i32", "_body.i16", ".i32", ".i16", ".wav", ".raw"}) {
    const size_t n = std::strlen(suf);
    if (base.size() > n && base.compare(base.size() - n, n, suf) == 0) {
      base.resize(base.size() - n);
      break;
    }
  }
  slot.label = base.empty() ? "slot" : base;
}

const Slot &Client::GetSlot(uint8_t voice) const
{
  static const Slot kEmpty{};
  if (voice >= cardlink::audio::kSampleVoices) {
    return kEmpty;
  }
  return slots_[voice];
}

bool Client::LoadWave(uint8_t voice, const std::string &path, std::string &err)
{
  if (voice >= cardlink::audio::kSampleVoices || path.empty()) {
    err = "err: voice 0..7";
    return false;
  }
  cardlink::audio::LoadedWave wave;
  if (!cardlink::audio::LoadWaveFile(path, 0, wave, err)) {
    return false;
  }
  auto &slot = slots_[voice];
  slot.head_path = path;
  slot.body_path = path;
  SetLabel(voice);
  if (!UploadAttack(voice, wave.attack.data(), wave.attack.size(), err)) {
    slot.head_on_card = false;
    slot.body_ready = false;
    return false;
  }
  slot.head_on_card = true;
  if (!mixer_.SetBody(voice, wave.body.data(), wave.body.size(), err)) {
    slot.body_ready = false;
    return false;
  }
  slot.body_ready = true;
  char aw[32];
  std::snprintf(aw, sizeof aw, "aw %u %u", static_cast<unsigned>(voice),
                static_cast<unsigned>(voice));
  SendConsole(aw);
  return true;
}

bool Client::LoadHead(uint8_t voice, const std::string &path, std::string &err)
{
  if (voice >= cardlink::audio::kSampleVoices || path.empty()) {
    err = "err: voice 0..7";
    return false;
  }
  if (IsWaveFile(path)) {
    return LoadWave(voice, path, err);
  }
  auto &slot = slots_[voice];
  slot.head_path = path;
  SetLabel(voice);
  if (!UploadAttackFile(voice, path, err)) {
    slot.head_on_card = false;
    return false;
  }
  slot.head_on_card = true;
  char aw[32];
  std::snprintf(aw, sizeof aw, "aw %u %u", static_cast<unsigned>(voice),
                static_cast<unsigned>(voice));
  SendConsole(aw);
  return true;
}

bool Client::LoadBody(uint8_t voice, const std::string &path, std::string &err)
{
  if (voice >= cardlink::audio::kSampleVoices || path.empty()) {
    err = "err: voice 0..7";
    return false;
  }
  if (IsWaveFile(path)) {
    return LoadWave(voice, path, err);
  }
  auto &slot = slots_[voice];
  slot.body_path = path;
  if (slot.label.empty()) {
    SetLabel(voice);
  }
  std::vector<int16_t> body;
  if (!slot.head_path.empty() &&
      cardlink::audio::BodyWithHeadOverlap(slot.head_path, path, body, err)) {
    if (!mixer_.SetBody(voice, body.data(), body.size(), err)) {
      slot.body_ready = false;
      return false;
    }
    {
      std::error_code ec;
      const auto bytes = fs::file_size(slot.head_path, ec);
      if (!ec && bytes >= 4u && (bytes % 4u) == 0u) {
        mixer_.SetAttackLen(voice, static_cast<unsigned>(bytes / 4u));
      }
    }
    slot.body_ready = true;
    return true;
  }
  if (!mixer_.LoadBodyFile(voice, path, err)) {
    slot.body_ready = false;
    return false;
  }
  slot.body_ready = true;
  return true;
}

bool Client::SetRootHz(uint8_t voice, double hz, std::string &err)
{
  if (voice >= cardlink::audio::kSampleVoices || !(hz > 0.0)) {
    err = "err: range";
    return false;
  }
  mixer_.SetBodyRootHz(voice, hz);
  char cmd[64];
  std::snprintf(cmd, sizeof cmd, "c:ar %u %.6g\r",
                static_cast<unsigned>(voice), hz);
  return SendCdcLine(cmd, err);
}

int Client::LoadFolder(const std::string &dir, std::string &err)
{
  if (cdc_path_.empty()) {
    err = "err: CDC path not set";
    return 0;
  }
  const fs::path root(dir);
  int loaded = 0;
  bool any_named = false;
  for (unsigned i = 0; i < cardlink::audio::kSampleVoices; ++i) {
    std::string head;
    std::string body;
    if (!FindNamedPair(root, static_cast<int>(i), head, body)) {
      continue;
    }
    any_named = true;
    if (!LoadHead(static_cast<uint8_t>(i), head, err) ||
        !LoadBody(static_cast<uint8_t>(i), body, err)) {
      return loaded;
    }
    ++loaded;
  }
  if (!any_named) {
    std::array<std::string, cardlink::audio::kSampleVoices> heads{};
    std::array<std::string, cardlink::audio::kSampleVoices> bodies{};
    if (!FindStemPairs(root, heads, bodies)) {
      err = "err: folder has no wN_*_head.i32 / *_body.i16 pairs";
      return 0;
    }
    for (unsigned i = 0; i < cardlink::audio::kSampleVoices; ++i) {
      if (heads[i].empty()) {
        continue;
      }
      if (!LoadHead(static_cast<uint8_t>(i), heads[i], err) ||
          !LoadBody(static_cast<uint8_t>(i), bodies[i], err)) {
        return loaded;
      }
      ++loaded;
    }
  }
  const fs::path roots = root / "roots.txt";
  if (fs::exists(roots)) {
    std::string roots_err;
    (void)mixer_.LoadRootsFile(roots.string(), roots_err);
    std::ifstream in(roots.string());
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      unsigned id = 0;
      double hz = 0.0;
      if (std::sscanf(line.c_str(), "%u %lf", &id, &hz) == 2 &&
          id < cardlink::audio::kSampleVoices && hz > 0.0) {
        std::string ignore;
        (void)SetRootHz(static_cast<uint8_t>(id), hz, ignore);
      }
    }
  }
  return loaded;
}

void Client::NoteOn(uint8_t voice, double hz)
{
  if (voice >= cardlink::audio::kSampleVoices) {
    return;
  }
  mixer_.NoteOn(voice, voice, hz);
  char cmd[48];
  std::snprintf(cmd, sizeof cmd, "n%u %.6g", static_cast<unsigned>(voice), hz);
  SendConsole(cmd);
}

void Client::NoteOff(uint8_t voice)
{
  if (voice >= cardlink::audio::kSampleVoices) {
    return;
  }
  char cmd[24];
  std::snprintf(cmd, sizeof cmd, "n%u 0", static_cast<unsigned>(voice));
  SendConsole(cmd);
  mixer_.NoteOff(voice);
}

void Client::AllNotesOff()
{
  mixer_.AllNotesOff();
  SendConsole("n 0");
}

void Client::Silence(uint8_t voice)
{
  mixer_.Silence(voice);
}

} // namespace sample
} // namespace cardlink
