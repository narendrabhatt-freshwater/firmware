#include "cardlink/sample/client.hpp"

#include "cardlink/audio/wave_loader.hpp"
#include "cardlink/serial_port.hpp"
#include "cardlink/usb/attack_upload.hpp"
#include "cardlink/usb/cdc_port.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
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

bool ParseWId(const std::string &name, int &id)
{
  if (name.size() < 3 || name[0] != 'w') {
    return false;
  }
  if (name[1] < '0' || name[1] > '9') {
    return false;
  }
  int n = 0;
  size_t i = 1;
  while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
    n = n * 10 + (name[i] - '0');
    ++i;
  }
  if (i >= name.size() || name[i] != '_') {
    return false;
  }
  id = n;
  return true;
}

bool FindNamedPair(const fs::path &dir, int id, std::string &head,
                   std::string &body)
{
  head.clear();
  body.clear();
  try {
    if (!fs::exists(dir)) {
      return false;
    }
    for (const auto &ent : fs::directory_iterator(dir)) {
      if (!ent.is_regular_file()) {
        continue;
      }
      const auto name = ent.path().filename().string();
      int wid = -1;
      if (!ParseWId(name, wid) || wid != id) {
        continue;
      }
      if (name.find("_head.i32") != std::string::npos ||
          name.find("_head.i16") != std::string::npos) {
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
                   std::array<std::string, cardlink::audio::kAttackWaves> &heads,
                   std::array<std::string, cardlink::audio::kAttackWaves> &bodies)
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
          (name.compare(name.size() - 9, 9, "_head.i32") == 0 ||
           name.compare(name.size() - 9, 9, "_head.i16") == 0)) {
        head_files.push_back(ent.path());
      }
    }
  } catch (...) {
    return false;
  }
  std::sort(head_files.begin(), head_files.end());
  int n = 0;
  for (const auto &hp : head_files) {
    if (n >= static_cast<int>(cardlink::audio::kAttackWaves)) {
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

struct CdcHold {
  Client &c;
  bool ok;
  CdcHold(Client &client, std::string &err) : c(client), ok(c.BeginCdc(err)) {}
  ~CdcHold()
  {
    if (ok) {
      c.EndCdc();
    }
  }
  CdcHold(const CdcHold &) = delete;
  CdcHold &operator=(const CdcHold &) = delete;
  explicit operator bool() const { return ok; }
};

} // namespace

void Client::SetConsole(ConsoleFn fn)
{
  console_ = std::move(fn);
}

void Client::SetCdcPath(const std::string &path)
{
  if (path == cdc_path_) {
    return;
  }
  if (cdc_port_.IsOpen()) {
    cdc_port_.Close();
    cdc_refs_ = 0;
  }
  cdc_path_ = path;
}

bool Client::BeginCdc(std::string &err)
{
  if (cdc_path_.empty()) {
    err = "err: CDC path not set";
    return false;
  }
  if (cdc_port_.IsOpen()) {
    ++cdc_refs_;
    return true;
  }
  if (!cardlink::usb::OpenCdcPort(cdc_port_, cdc_path_, err)) {
    return false;
  }
  cdc_refs_ = 1;
  return true;
}

void Client::EndCdc()
{
  if (cdc_refs_ <= 0) {
    return;
  }
  --cdc_refs_;
  if (cdc_refs_ == 0 && cdc_port_.IsOpen()) {
    cdc_port_.Close();
  }
}

void Client::SendConsole(const std::string &cmd)
{
  if (console_) {
    console_(cmd);
  }
}

bool Client::SendCdcLine(const std::string &line, std::string &err)
{
  if (!BeginCdc(err)) {
    return false;
  }
  const bool wrote =
      cdc_port_.Write(reinterpret_cast<const uint8_t *>(line.data()),
                      line.size());
  if (wrote) {
    cdc_port_.DrainOutput();
    uint8_t buf[128];
    (void)cdc_port_.ReadTimeout(buf, sizeof(buf), 200);
  } else {
    err = "err: CDC write";
  }
  EndCdc();
  return wrote;
}

bool Client::UploadAttack(uint16_t wave_id, const int16_t *q15, size_t nsamp,
                          std::string &err)
{
  if (wave_id >= cardlink::audio::kAttackWaves || q15 == nullptr ||
      nsamp == 0 || nsamp > cardlink::audio::kAttackSamples) {
    err = "err: attack must be 1..512 int16 samples";
    return false;
  }
  if (!BeginCdc(err)) {
    return false;
  }
  cardlink::usb::AttackUploader up(cdc_port_);
  auto r = up.Upload(wave_id, reinterpret_cast<const uint8_t *>(q15),
                     nsamp * sizeof(int16_t));
  EndCdc();
  if (!r.ok) {
    err = r.message;
    return false;
  }
  mixer_.SetAttackLen(wave_id, static_cast<unsigned>(nsamp));
  return true;
}

bool Client::UploadAttackFile(uint16_t wave_id, const std::string &path,
                              std::string &err)
{
  if (!BeginCdc(err)) {
    return false;
  }
  cardlink::usb::AttackUploader up(cdc_port_);
  auto r = up.UploadFile(wave_id, path);
  EndCdc();
  if (!r.ok) {
    err = r.message;
    return false;
  }
  std::error_code ec;
  const auto bytes = fs::file_size(path, ec);
  if (!ec) {
    const bool i32 = (bytes % 4u) == 0u &&
                     (bytes > cardlink::audio::kAttackSamples * 2u ||
                      path.find(".i32") != std::string::npos);
    const unsigned nsamp = i32 ? static_cast<unsigned>(bytes / 4u)
                               : static_cast<unsigned>(bytes / 2u);
    mixer_.SetAttackLen(
        wave_id, std::min(nsamp, cardlink::audio::kAttackSamples));
  }
  return true;
}

void Client::SetLabel(uint16_t wave_id)
{
  if (wave_id >= cardlink::audio::kSampleVoices) {
    return;
  }
  auto &slot = slots_[static_cast<size_t>(wave_id)];
  std::string base = Basename(slot.head_path);
  if (base.empty()) {
    base = Basename(slot.body_path);
  }
  int wid = -1;
  if (ParseWId(base, wid) && base.size() > 2) {
    const auto us = base.find('_');
    if (us != std::string::npos) {
      base = base.substr(us + 1);
    }
  }
  for (const char *suf :
       {"_head.i32", "_head.i16", "_body.i16", ".i32", ".i16", ".wav", ".raw"}) {
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

bool Client::LoadWave(uint16_t wave_id, const std::string &path, std::string &err)
{
  if (wave_id >= cardlink::audio::kAttackWaves || path.empty()) {
    err = "err: wave_id 0..255";
    return false;
  }
  cardlink::audio::LoadedWave wave;
  if (!cardlink::audio::LoadWaveFile(path, 0, wave, err)) {
    return false;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    auto &slot = slots_[static_cast<size_t>(wave_id)];
    slot.head_path = path;
    slot.body_path = path;
    SetLabel(wave_id);
  }
  if (!UploadAttack(wave_id, wave.attack.data(), wave.attack.size(), err)) {
    if (wave_id < cardlink::audio::kSampleVoices) {
      slots_[static_cast<size_t>(wave_id)].head_on_card = false;
      slots_[static_cast<size_t>(wave_id)].body_ready = false;
    }
    return false;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    slots_[static_cast<size_t>(wave_id)].head_on_card = true;
  }
  if (!mixer_.SetBody(wave_id, wave.body.data(), wave.body.size(), err)) {
    if (wave_id < cardlink::audio::kSampleVoices) {
      slots_[static_cast<size_t>(wave_id)].body_ready = false;
    }
    return false;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    slots_[static_cast<size_t>(wave_id)].body_ready = true;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    char aw[32];
    std::snprintf(aw, sizeof aw, "aw %u %u", static_cast<unsigned>(wave_id),
                  static_cast<unsigned>(wave_id));
    SendConsole(aw);
  }
  return true;
}

bool Client::LoadHead(uint16_t wave_id, const std::string &path, std::string &err)
{
  if (wave_id >= cardlink::audio::kAttackWaves || path.empty()) {
    err = "err: wave_id 0..255";
    return false;
  }
  if (IsWaveFile(path)) {
    return LoadWave(wave_id, path, err);
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    slots_[static_cast<size_t>(wave_id)].head_path = path;
    SetLabel(wave_id);
  }
  if (!UploadAttackFile(wave_id, path, err)) {
    if (wave_id < cardlink::audio::kSampleVoices) {
      slots_[static_cast<size_t>(wave_id)].head_on_card = false;
    }
    return false;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    slots_[static_cast<size_t>(wave_id)].head_on_card = true;
    char aw[32];
    std::snprintf(aw, sizeof aw, "aw %u %u", static_cast<unsigned>(wave_id),
                  static_cast<unsigned>(wave_id));
    SendConsole(aw);
  }
  return true;
}

bool Client::LoadBody(uint16_t wave_id, const std::string &path, std::string &err)
{
  if (wave_id >= cardlink::audio::kAttackWaves || path.empty()) {
    err = "err: wave_id 0..255";
    return false;
  }
  if (IsWaveFile(path)) {
    return LoadWave(wave_id, path, err);
  }
  std::string head_path;
  if (wave_id < cardlink::audio::kSampleVoices) {
    auto &slot = slots_[static_cast<size_t>(wave_id)];
    slot.body_path = path;
    if (slot.label.empty()) {
      SetLabel(wave_id);
    }
    head_path = slot.head_path;
  }
  std::vector<int16_t> body;
  if (!head_path.empty() &&
      cardlink::audio::BodyWithHeadOverlap(head_path, path, body, err)) {
    if (!mixer_.SetBody(wave_id, body.data(), body.size(), err)) {
      if (wave_id < cardlink::audio::kSampleVoices) {
        slots_[static_cast<size_t>(wave_id)].body_ready = false;
      }
      return false;
    }
    {
      std::error_code ec;
      const auto bytes = fs::file_size(head_path, ec);
      if (!ec && bytes >= 2u) {
        const bool i32 = (bytes % 4u) == 0u &&
                         (bytes > cardlink::audio::kAttackSamples * 2u ||
                          head_path.find(".i32") != std::string::npos);
        const unsigned nsamp = i32 ? static_cast<unsigned>(bytes / 4u)
                                   : static_cast<unsigned>(bytes / 2u);
        mixer_.SetAttackLen(
            wave_id, std::min(nsamp, cardlink::audio::kAttackSamples));
      }
    }
    if (wave_id < cardlink::audio::kSampleVoices) {
      slots_[static_cast<size_t>(wave_id)].body_ready = true;
    }
    return true;
  }
  if (!mixer_.LoadBodyFile(wave_id, path, err)) {
    if (wave_id < cardlink::audio::kSampleVoices) {
      slots_[static_cast<size_t>(wave_id)].body_ready = false;
    }
    return false;
  }
  if (wave_id < cardlink::audio::kSampleVoices) {
    slots_[static_cast<size_t>(wave_id)].body_ready = true;
  }
  return true;
}

bool Client::SetRootHz(uint16_t wave_id, double hz, std::string &err)
{
  if (wave_id >= cardlink::audio::kAttackWaves || !(hz > 0.0)) {
    err = "err: range";
    return false;
  }
  mixer_.SetBodyRootHz(wave_id, hz);
  char cmd[64];
  std::snprintf(cmd, sizeof cmd, "c:ar %u %.6g\r",
                static_cast<unsigned>(wave_id), hz);
  return SendCdcLine(cmd, err);
}

int Client::LoadFolder(const std::string &dir, std::string &err)
{
  CdcHold cdc(*this, err);
  if (!cdc) {
    return 0;
  }
  const fs::path root(dir);
  int loaded = 0;
  bool any_named = false;
  for (unsigned i = 0; i < cardlink::audio::kAttackWaves; ++i) {
    std::string head;
    std::string body;
    if (!FindNamedPair(root, static_cast<int>(i), head, body)) {
      continue;
    }
    any_named = true;
    if (!LoadHead(static_cast<uint16_t>(i), head, err) ||
        !LoadBody(static_cast<uint16_t>(i), body, err)) {
      return loaded;
    }
    ++loaded;
  }
  if (!any_named) {
    std::array<std::string, cardlink::audio::kAttackWaves> heads{};
    std::array<std::string, cardlink::audio::kAttackWaves> bodies{};
    if (!FindStemPairs(root, heads, bodies)) {
      err = "err: folder has no wN_*_head.i32/.i16 / *_body.i16 pairs";
      return 0;
    }
    for (unsigned i = 0; i < cardlink::audio::kAttackWaves; ++i) {
      if (heads[i].empty()) {
        continue;
      }
      if (!LoadHead(static_cast<uint16_t>(i), heads[i], err) ||
          !LoadBody(static_cast<uint16_t>(i), bodies[i], err)) {
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
          id < cardlink::audio::kAttackWaves && hz > 0.0) {
        std::string ignore;
        (void)SetRootHz(static_cast<uint16_t>(id), hz, ignore);
      }
    }
  }
  return loaded;
}

void Client::NoteOn(uint8_t voice, double hz, uint16_t wave_id)
{
  const NoteRequest note{voice, hz, wave_id};
  (void)NoteOnBatch(&note, 1u);
}

bool Client::NoteOnBatch(const NoteRequest *notes, size_t count,
                         unsigned legacy_timeout_ms)
{
  /* Kept in the ABI for existing callers. Note-on does not wait for BODY:
   * the card's attack head bridges to the first vq-authorized UAC PACK. */
  (void)legacy_timeout_ms;
  if (notes == nullptr || count == 0u || count > cardlink::audio::kSampleVoices) {
    return false;
  }
  std::array<NoteRequest, cardlink::audio::kSampleVoices> prepared{};
  std::array<bool, cardlink::audio::kSampleVoices> seen{};
  size_t nprepared = 0u;
  for (size_t ni = 0u; ni < count; ++ni) {
    const auto &note = notes[ni];
    if (note.voice >= cardlink::audio::kSampleVoices || !(note.hz > 0.0) ||
        seen[note.voice]) {
      continue;
    }
    seen[note.voice] = true;
    uint16_t wave = note.wave_id;
    if (wave >= cardlink::audio::kAttackWaves) {
      wave = note.voice;
      if (!slots_[note.voice].body_ready) {
        for (uint16_t i = 0; i < cardlink::audio::kAttackWaves; ++i) {
          if (mixer_.HasBody(i)) {
            wave = i;
            break;
          }
        }
      }
    } else if (!mixer_.HasBody(wave)) {
      for (uint16_t i = 0; i < cardlink::audio::kAttackWaves; ++i) {
        if (mixer_.HasBody(i)) {
          wave = i;
          break;
        }
      }
    }
    if (!mixer_.HasBody(wave)) {
      continue;
    }
    char aw[32];
    std::snprintf(aw, sizeof aw, "aw %u %u",
                  static_cast<unsigned>(note.voice),
                  static_cast<unsigned>(wave));
    SendConsole(aw);
    mixer_.NoteOn(note.voice, wave, note.hz);
    prepared[nprepared++] = NoteRequest{note.voice, note.hz, wave};
  }
  if (nprepared == 0u) {
    return false;
  }

  /* Start the chord immediately. The card plays its attack heads while the
   * BODY thread waits for the next vq, then sends one permitted UAC PACK. */
  for (size_t i = 0u; i < nprepared; ++i) {
    char cmd[48];
    std::snprintf(cmd, sizeof cmd, "n%u %.6g",
                  static_cast<unsigned>(prepared[i].voice), prepared[i].hz);
    SendConsole(cmd);
  }
  return true;
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
