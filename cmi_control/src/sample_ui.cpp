#include "sample_ui.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "cardlink/audio/wave_loader.hpp"
#include "cardlink/usb/attack_upload.hpp"
#include "cardlink/usb/wave_upload.hpp"
#include "cardproto/types.hpp"

#include "imgui.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using fw::theme::kPalette;
using fw::theme::S;
using fw::theme::S2;
using fw::ui::BtnKind;

namespace {

constexpr int kVoices = 8;

const double kChordHz[kVoices] = {
    261.63, 329.63, 392.00, 523.25, 659.25, 783.99, 1046.50, 1318.51,
};

void MonoText(const char *text, const ImVec4 &col, ImFont *font)
{
  if (font) {
    ImGui::PushFont(font);
  }
  ImGui::TextColored(col, "%s", text);
  if (font) {
    ImGui::PopFont();
  }
}

std::string Basename(const std::string &path)
{
  if (path.empty()) {
    return {};
  }
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

void SetLabelFromHead(SampleSlotState &slot)
{
  std::string base = Basename(slot.head_path);
  if (base.empty()) {
    base = Basename(slot.body_path);
  }
  // w0_saw_head.i32 → saw
  if (base.size() > 3 && base[0] == 'w' && base[1] >= '0' && base[1] <= '7' &&
      base[2] == '_') {
    base = base.substr(3);
  }
  for (const char *suf :
       {"_head.i32", "_body.i16", ".i32", ".i16", ".wav", ".raw"}) {
    const std::size_t n = std::strlen(suf);
    if (base.size() > n &&
        base.compare(base.size() - n, n, suf) == 0) {
      base.resize(base.size() - n);
      break;
    }
  }
  if (base.empty()) {
    std::snprintf(slot.label, sizeof(slot.label), "slot");
  } else {
    std::snprintf(slot.label, sizeof(slot.label), "%s", base.c_str());
  }
}

bool UploadHeadToCard(App &app, uint16_t id, const std::string &path)
{
  std::string cdc_err;
  if (!app.EnsureAttackCdc(cdc_err)) {
    app.log.Push(cdc_err);
    app.PushToastErr(cdc_err);
    return false;
  }
  cardlink::SerialPort port;
  std::string err;
  if (!cardlink::usb::WaveUploader::OpenCdcPort(port, app.wave_cdc_path, err)) {
    app.log.Push(err);
    app.PushToastErr(err);
    return false;
  }
  cardlink::usb::AttackUploader up(port);
  auto r = up.UploadFile(id, path);
  if (!r.ok) {
    app.log.Push(r.message);
    app.PushToastErr(r.message);
    return false;
  }
  char msg[48];
  std::snprintf(msg, sizeof msg, "ok: attack head w%u → card",
                static_cast<unsigned>(id));
  app.log.Push(msg);
  return true;
}

bool HasExtI(const std::string &path, const char *ext)
{
  const std::size_t n = std::strlen(ext);
  if (path.size() < n) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char a =
        static_cast<unsigned char>(path[path.size() - n + i]);
    const unsigned char b = static_cast<unsigned char>(ext[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

bool LooksLikeWaveFile(const std::string &path)
{
  return HasExtI(path, ".wav") || HasExtI(path, ".raw");
}

bool UploadAttackBytes(App &app, uint16_t id, const int32_t *q31,
                       std::size_t nsamp)
{
  if (q31 == nullptr || nsamp != cardlink::audio::kAttackSamples) {
    app.log.Push("err: attack must be 256 Q31 samples");
    app.PushToastErr("attack must be 256 samples");
    return false;
  }
  std::string cdc_err;
  if (!app.EnsureAttackCdc(cdc_err)) {
    app.log.Push(cdc_err);
    app.PushToastErr(cdc_err);
    return false;
  }
  cardlink::SerialPort port;
  std::string err;
  if (!cardlink::usb::WaveUploader::OpenCdcPort(port, app.wave_cdc_path, err)) {
    app.log.Push(err);
    app.PushToastErr(err);
    return false;
  }
  cardlink::usb::AttackUploader up(port);
  auto r = up.Upload(id, reinterpret_cast<const uint8_t *>(q31),
                     nsamp * sizeof(int32_t));
  if (!r.ok) {
    app.log.Push(r.message);
    app.PushToastErr(r.message);
    return false;
  }
  char msg[48];
  std::snprintf(msg, sizeof msg, "ok: attack head w%u → card",
                static_cast<unsigned>(id));
  app.log.Push(msg);
  return true;
}

bool SetRootOnCard(App &app, uint16_t id, double hz)
{
  std::string cdc_err;
  if (!app.EnsureAttackCdc(cdc_err)) {
    return false;
  }
  cardlink::SerialPort port;
  std::string err;
  if (!cardlink::usb::WaveUploader::OpenCdcPort(port, app.wave_cdc_path, err)) {
    return false;
  }
  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "c:ar %u %.6g\r",
                static_cast<unsigned>(id), hz);
  if (!port.Write(reinterpret_cast<const uint8_t *>(cmd), std::strlen(cmd))) {
    return false;
  }
  port.DrainOutput();
  /* Best-effort; card replies ok:ar */
  uint8_t buf[128];
  (void)port.ReadTimeout(buf, sizeof(buf), 200);
  return true;
}

bool LoadBodyToMixer(App &app, uint16_t id, const std::string &path)
{
  if (!app.sample_uac) {
    app.sample_uac = std::make_unique<SampleUacOut>();
  }
  std::string err;
  auto &slot = app.sample_slots[id];
  if (slot.head_path[0] != '\0') {
    std::vector<int16_t> body;
    if (cardlink::audio::BodyWithHeadOverlap(slot.head_path, path, body, err)) {
      if (!app.sample_uac->Mixer().SetBody(id, body.data(), body.size(), err)) {
        app.log.Push("err: " + err);
        app.PushToastErr(err);
        return false;
      }
      return true;
    }
  }
  if (!app.sample_uac->Mixer().LoadBodyFile(id, path, err)) {
    app.log.Push("err: " + err);
    app.PushToastErr(err);
    return false;
  }
  return true;
}

bool AssignAw(App &app, int voice, uint16_t wave_id)
{
  char cmd[48];
  std::snprintf(cmd, sizeof cmd, "aw %d %u", voice,
                static_cast<unsigned>(wave_id));
  return app.bus.QueueExec(cardproto::Target::Channel, cmd) ==
         BusQueueResult::Ok;
}

bool FindPairInDir(const fs::path &dir, int id, std::string &head,
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

/** Fallback: sorted *_head.i32 / matching *_body.i16 by stem. */
bool FindPairsFallback(const fs::path &dir,
                       std::array<std::string, kVoices> &heads,
                       std::array<std::string, kVoices> &bodies)
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
    if (n >= kVoices) {
      break;
    }
    const std::string name = hp.filename().string();
    const std::string stem = name.substr(0, name.size() - 9); // drop _head.i32
    const fs::path bp = hp.parent_path() / (stem + "_body.i16");
    if (!fs::exists(bp)) {
      continue;
    }
    heads[static_cast<std::size_t>(n)] = hp.string();
    bodies[static_cast<std::size_t>(n)] = bp.string();
    ++n;
  }
  return n > 0;
}

bool ApplyWaveFile(App &app, int voice, const std::string &path)
{
  if (voice < 0 || voice >= kVoices || path.empty()) {
    return false;
  }
  cardlink::audio::LoadedWave wave;
  std::string err;
  if (!cardlink::audio::LoadWaveFile(path, 0, wave, err)) {
    app.log.Push("err: " + err);
    app.PushToastErr(err);
    return false;
  }
  auto &slot = app.sample_slots[static_cast<std::size_t>(voice)];
  std::snprintf(slot.head_path, sizeof(slot.head_path), "%s", path.c_str());
  std::snprintf(slot.body_path, sizeof(slot.body_path), "%s", path.c_str());
  SetLabelFromHead(slot);

  const uint16_t id = static_cast<uint16_t>(voice);
  if (!UploadAttackBytes(app, id, wave.attack.data(), wave.attack.size())) {
    slot.head_on_card = false;
    slot.body_in_mixer = false;
    return false;
  }
  slot.head_on_card = true;

  if (!app.sample_uac) {
    app.sample_uac = std::make_unique<SampleUacOut>();
  }
  if (!app.sample_uac->Mixer().SetBody(id, wave.body.data(), wave.body.size(),
                                       err)) {
    slot.body_in_mixer = false;
    app.log.Push("err: " + err);
    app.PushToastErr(err);
    return false;
  }
  slot.body_in_mixer = true;
  AssignAw(app, voice, id);
  char msg[64];
  std::snprintf(msg, sizeof msg, "ok: wav w%u SRC 48 kHz → card + UAC",
                static_cast<unsigned>(id));
  app.log.Push(msg);
  app.PushToastOk(msg);
  return true;
}

bool ApplyHead(App &app, int voice, const std::string &path)
{
  if (voice < 0 || voice >= kVoices || path.empty()) {
    return false;
  }
  if (LooksLikeWaveFile(path)) {
    return ApplyWaveFile(app, voice, path);
  }
  auto &slot = app.sample_slots[static_cast<std::size_t>(voice)];
  std::snprintf(slot.head_path, sizeof(slot.head_path), "%s", path.c_str());
  SetLabelFromHead(slot);
  if (!UploadHeadToCard(app, static_cast<uint16_t>(voice), path)) {
    slot.head_on_card = false;
    return false;
  }
  slot.head_on_card = true;
  AssignAw(app, voice, static_cast<uint16_t>(voice));
  return true;
}

bool ApplyBody(App &app, int voice, const std::string &path)
{
  if (voice < 0 || voice >= kVoices || path.empty()) {
    return false;
  }
  if (LooksLikeWaveFile(path)) {
    return ApplyWaveFile(app, voice, path);
  }
  auto &slot = app.sample_slots[static_cast<std::size_t>(voice)];
  std::snprintf(slot.body_path, sizeof(slot.body_path), "%s", path.c_str());
  if (slot.label[0] == '\0') {
    SetLabelFromHead(slot);
  }
  /* Bodies stay on the host for UAC dry — not CDC bl to the card. */
  if (!LoadBodyToMixer(app, static_cast<uint16_t>(voice), path)) {
    slot.body_in_mixer = false;
    return false;
  }
  slot.body_in_mixer = true;
  char msg[48];
  std::snprintf(msg, sizeof msg, "ok: body w%u → UAC mixer",
                static_cast<unsigned>(voice));
  app.log.Push(msg);
  return true;
}

int LoadFolder(App &app, const std::string &folder)
{
  std::string cdc_err;
  if (!app.EnsureAttackCdc(cdc_err)) {
    app.log.Push(cdc_err);
    app.PushToastErr(cdc_err);
    return 0;
  }

  const fs::path dir(folder);
  int loaded = 0;
  bool any_named = false;
  for (int i = 0; i < kVoices; ++i) {
    std::string head;
    std::string body;
    if (FindPairInDir(dir, i, head, body)) {
      any_named = true;
      if (!ApplyHead(app, i, head)) {
        app.log.Push("err: stopped folder load (attack CDC failed)");
        return loaded;
      }
      if (!ApplyBody(app, i, body)) {
        app.log.Push("err: stopped folder load (body load failed)");
        return loaded;
      }
      ++loaded;
    }
  }
  if (!any_named) {
    std::array<std::string, kVoices> heads{};
    std::array<std::string, kVoices> bodies{};
    if (!FindPairsFallback(dir, heads, bodies)) {
      app.log.Push("err: folder has no wN_*_head.i32 / *_body.i16 pairs");
      app.PushToastErr("no sample pairs in folder");
      return 0;
    }
    for (int i = 0; i < kVoices; ++i) {
      if (heads[static_cast<std::size_t>(i)].empty()) {
        continue;
      }
      if (!ApplyHead(app, i, heads[static_cast<std::size_t>(i)])) {
        app.log.Push("err: stopped folder load (attack CDC failed)");
        return loaded;
      }
      if (!ApplyBody(app, i, bodies[static_cast<std::size_t>(i)])) {
        app.log.Push("err: stopped folder load (body load failed)");
        return loaded;
      }
      ++loaded;
    }
  }
  if (app.sample_uac) {
    std::string roots_err;
    const fs::path roots = dir / "roots.txt";
    if (fs::exists(roots)) {
      if (!app.sample_uac->Mixer().LoadRootsFile(roots.string(), roots_err)) {
        app.log.Push("err: " + roots_err);
      } else {
        std::ifstream in(roots.string());
        std::string line;
        while (std::getline(in, line)) {
          if (line.empty() || line[0] == '#') {
            continue;
          }
          unsigned id = 0;
          double hz = 0.0;
          if (std::sscanf(line.c_str(), "%u %lf", &id, &hz) == 2 && id < 8u &&
              hz > 0.0) {
            (void)SetRootOnCard(app, static_cast<uint16_t>(id), hz);
          }
        }
      }
    }
  }
  char msg[72];
  std::snprintf(msg, sizeof msg, "ok: loaded %d/%d from folder", loaded,
                kVoices);
  app.log.Push(msg);
  if (loaded > 0) {
    app.PushToastOk(msg);
  } else {
    app.PushToastErr(msg);
  }
  return loaded;
}

void DrainPending(App &app)
{
  if (app.pending_sample_folder.empty()) {
    return;
  }
  const std::string pending = std::move(app.pending_sample_folder);
  app.pending_sample_folder.clear();

  if (pending.rfind("head:", 0) == 0) {
    // head:<voice>:<path>
    const auto c1 = pending.find(':', 5);
    if (c1 == std::string::npos) {
      return;
    }
    const int voice = std::atoi(pending.c_str() + 5);
    ApplyHead(app, voice, pending.substr(c1 + 1));
    return;
  }
  if (pending.rfind("body:", 0) == 0) {
    const auto c1 = pending.find(':', 5);
    if (c1 == std::string::npos) {
      return;
    }
    const int voice = std::atoi(pending.c_str() + 5);
    ApplyBody(app, voice, pending.substr(c1 + 1));
    return;
  }
  if (pending.rfind("wave:", 0) == 0) {
    const auto c1 = pending.find(':', 5);
    if (c1 == std::string::npos) {
      return;
    }
    const int voice = std::atoi(pending.c_str() + 5);
    ApplyWaveFile(app, voice, pending.substr(c1 + 1));
    return;
  }
  LoadFolder(app, pending);
}

void NoteOn(App &app, int voice, double hz)
{
  /* SOF + prefill must reach the ring before nX arms consume. */
  (void)app.EnsureSampleUac();
  if (app.sample_uac) {
    app.sample_uac->Mixer().NoteOn(static_cast<uint8_t>(voice),
                                   static_cast<uint16_t>(voice), hz);
  }
  char cmd[48];
  std::snprintf(cmd, sizeof cmd, "n%d %.2f", voice, hz);
  app.bus.QueueExec(cardproto::Target::Channel, cmd);
}

void NoteOff(App &app, int voice)
{
  char cmd[48];
  std::snprintf(cmd, sizeof cmd, "n%d 0", voice);
  app.bus.QueueExec(cardproto::Target::Channel, cmd);
  if (app.sample_uac) {
    app.sample_uac->Mixer().NoteOff(static_cast<uint8_t>(voice));
  }
}

void BeginPick(App &app, SampleFilePick kind, int voice)
{
  if (app.file_dialog.Busy()) {
    app.PushToastErr("file dialog busy");
    return;
  }
  app.sample_file_pick = kind;
  app.sample_file_voice = voice;
  app.file_dialog_kind = (kind == SampleFilePick::Folder)
                             ? AsyncFileDialog::Kind::Folder
                             : AsyncFileDialog::Kind::File;
  app.file_dialog_slot = -1;
  const bool ok = (kind == SampleFilePick::Folder)
                      ? app.file_dialog.BeginPickFolder()
                      : app.file_dialog.BeginPickFile();
  if (!ok) {
    app.sample_file_pick = SampleFilePick::None;
    app.sample_file_voice = -1;
    app.PushToastErr("file dialog busy");
  }
}

void DrawVoiceCard(App &app, int voice, float card_w, float card_h)
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;
  const auto &slot = app.sample_slots[static_cast<std::size_t>(voice)];
  const bool dialog_busy = app.file_dialog.Busy();

  ImGui::PushID(voice);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, kPalette.border);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(10.f, 8.f));
  ImGui::BeginChild("voice", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  ImGui::PopStyleVar();

  char id[8];
  std::snprintf(id, sizeof id, "n%x", voice);
  MonoText(id, kPalette.accent, fm);
  ImGui::SameLine(0.f, S(8.f));
  MonoText(slot.label[0] ? slot.label : "empty",
           slot.label[0] ? kPalette.text : kPalette.muted, fs);

  {
    char status[64];
    std::snprintf(status, sizeof status, "atk %s  ·  body %s",
                  slot.head_on_card ? "card" : "—",
                  slot.body_in_mixer ? "UAC" : "—");
    MonoText(status,
             (slot.head_on_card && slot.body_in_mixer) ? kPalette.accent
                                                       : kPalette.muted,
             fs);
  }

  {
    const std::string head_bn = Basename(slot.head_path);
    const std::string body_bn = Basename(slot.body_path);
    ImGui::PushFont(fs);
    ImGui::PushTextWrapPos(card_w - S(16.f));
    ImGui::TextColored(kPalette.text_dim, "head: %s",
                       head_bn.empty() ? "—" : head_bn.c_str());
    ImGui::TextColored(kPalette.text_dim, "body: %s",
                       body_bn.empty() ? "—" : body_bn.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(dialog_busy);
  if (fw::ui::ChipBtn("Head", false, BtnKind::Neutral)) {
    BeginPick(app, SampleFilePick::Head, voice);
  }
  ImGui::SameLine(0.f, S(4.f));
  if (fw::ui::ChipBtn("Body", false, BtnKind::Neutral)) {
    BeginPick(app, SampleFilePick::Body, voice);
  }
  ImGui::SameLine(0.f, S(4.f));
  if (fw::ui::ChipBtn("Wave", false, BtnKind::Neutral)) {
    BeginPick(app, SampleFilePick::Wave, voice);
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0.f, S(8.f));
  ImGui::BeginDisabled(!slot.head_on_card && !slot.body_in_mixer);
  if (fw::ui::ChipBtn("Play", false, BtnKind::Primary)) {
    NoteOn(app, voice, 440.0);
  }
  ImGui::SameLine(0.f, S(4.f));
  if (fw::ui::ChipBtn("Off", false, BtnKind::Neutral)) {
    NoteOff(app, voice);
  }
  ImGui::EndDisabled();

  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopID();
}

} // namespace

void DrawSamplePage(App &app)
{
  DrainPending(app);
  /* Keep CDC list fresh — card paths change when USB re-enumerates. */
  if ((ImGui::GetFrameCount() % 120) == 0) {
    app.RefreshPortLists();
  }

  ImFont *fs = fw::theme::g_fonts.mono_small;
  const bool uac_on = app.sample_uac && app.sample_uac->Running();
  const bool bus_ok = app.bus.IsOpen() && !app.bus.BusFault();
  const bool cdc_ok = app.wave_cdc_path[0] != '\0';
  const bool dialog_busy = app.file_dialog.Busy();

  // ── Top bar: CDC + UAC
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.f), 0.f));
  ImGui::BeginChild("sample_bar", ImVec2(0, S(44.f)), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x, wp.y + wsz.y - 1.f),
                ImVec2(wp.x + wsz.x, wp.y + wsz.y - 1.f),
                fw::theme::U32(kPalette.border));

    const float mid_y = S(11.f);
    ImGui::SetCursorPos(ImVec2(S(16.f), mid_y + S(4.f)));
    fw::ui::StatusDot(3.f, cdc_ok ? kPalette.accent : kPalette.muted, cdc_ok);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText("ATTACK CDC", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y);
    ImGui::SetNextItemWidth(S(200.f));
    if (!app.serial_ports.empty()) {
      std::string preview =
          cdc_ok ? app.wave_cdc_path : "\u2014 select \u2014";
      ImGui::PushFont(fs);
      if (ImGui::BeginCombo("##sample_cdc", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(app.serial_ports.size()); ++i) {
          const auto &p = app.serial_ports[static_cast<std::size_t>(i)];
          if (ImGui::Selectable(p.c_str(), p == app.wave_cdc_path)) {
            app.wave_cdc_port_index = i;
            std::snprintf(app.wave_cdc_path, sizeof(app.wave_cdc_path), "%s",
                          p.c_str());
            app.MarkSettingsDirty();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopFont();
    } else {
      ImGui::PushFont(fs);
      if (ImGui::InputTextWithHint("##sample_cdc_path", "CDC path\u2026",
                                   app.wave_cdc_path,
                                   sizeof(app.wave_cdc_path))) {
        app.MarkSettingsDirty();
      }
      ImGui::PopFont();
    }

    ImGui::SameLine(0.f, S(14.f));
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(p.x, wp.y + S(12.f)), ImVec2(p.x, wp.y + S(28.f)),
                  fw::theme::U32(kPalette.border));
      ImGui::Dummy(ImVec2(1.f, 0.f));
    }

    ImGui::SameLine(0.f, S(14.f));
    ImGui::SetCursorPosY(mid_y + S(4.f));
    fw::ui::StatusDot(3.f, uac_on ? kPalette.accent : kPalette.muted, uac_on);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText(uac_on ? "UAC DRY ON" : "UAC DRY OFF",
             uac_on ? kPalette.accent : kPalette.text_dim, fs);

    ImGui::SameLine(0.f, S(12.f));
    ImGui::SetCursorPosY(mid_y);
    if (!uac_on) {
      if (fw::ui::Btn("Start UAC", ImVec2(0, S(22.f)), BtnKind::Primary)) {
        if (!app.sample_uac) {
          app.sample_uac = std::make_unique<SampleUacOut>();
        }
        std::string err;
        if (!app.sample_uac->Start("Channel Card", err)) {
          app.log.Push("err: " + err);
          app.PushToastErr(err);
        } else {
          app.log.Push("ok: UAC dry stream open");
          app.PushToastOk("UAC dry open");
        }
      }
    } else if (fw::ui::Btn("Stop UAC", ImVec2(0, S(22.f)), BtnKind::Neutral)) {
      app.sample_uac->Stop();
    }

    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText("48 kHz · 8ch int16", kPalette.muted, fs);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // ── Library / actions
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(16.f, 12.f));
  ImGui::BeginChild("sample_actions", ImVec2(0, S(76.f)),
                    ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  {
    MonoText("LIBRARY", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(12.f));
    MonoText("folder auto-load · Head/Body split files · Wave wav/raw", kPalette.muted,
             fs);

    ImGui::Spacing();
    ImGui::BeginDisabled(dialog_busy || !cdc_ok);
    if (fw::ui::Btn("Load folder\u2026", ImVec2(0, S(24.f)), BtnKind::Primary)) {
      BeginPick(app, SampleFilePick::Folder, -1);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (!cdc_ok) {
        ImGui::SetTooltip("Select Channel Card CDC port first");
      } else {
        ImGui::SetTooltip(
            "Pick a folder with w0_*_head.i32 + w0_*_body.i16 \u2026 w7");
      }
    }

    ImGui::SameLine(0.f, S(8.f));
    ImGui::BeginDisabled(dialog_busy || !cdc_ok);
    if (fw::ui::Btn("Load sample48", ImVec2(0, S(24.f)), BtnKind::Neutral)) {
      static const char *kCandidates[] = {
          "cmi_control/waves/sample48",
          "waves/sample48",
          "../waves/sample48",
          "../../cmi_control/waves/sample48",
      };
      bool found = false;
      for (const char *c : kCandidates) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) {
          LoadFolder(app, c);
          found = true;
          break;
        }
      }
      if (!found) {
        app.PushToastErr("sample48 folder not found");
      }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.f, S(12.f));
    ImGui::BeginDisabled(!bus_ok);
    if (fw::ui::Btn("8-note chord", ImVec2(0, S(24.f)), BtnKind::Primary)) {
      for (int v = 0; v < kVoices; ++v) {
        NoteOn(app, v, kChordHz[v]);
      }
    }
    ImGui::SameLine(0.f, S(6.f));
    if (fw::ui::Btn("All off", ImVec2(0, S(24.f)), BtnKind::Neutral)) {
      app.bus.RequestSilence();
      if (app.sample_uac) {
        app.sample_uac->Mixer().AllNotesOff();
      }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.f, S(16.f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5.f));
    if (!bus_ok) {
      MonoText("RS485 offline", kPalette.warning, fs);
    } else if (!uac_on) {
      MonoText("start UAC for sustain body", kPalette.muted, fs);
    } else {
      MonoText("card owns env · filter · mix", kPalette.muted, fs);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                fw::theme::U32(kPalette.border));
    ImGui::Dummy(ImVec2(0, 1.f));
  }

  // ── Voice grid
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(16.f, 16.f));
  ImGui::BeginChild("sample_grid", ImVec2(0, 0),
                    ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();

  MonoText("VOICES", kPalette.text_dim, fs);
  ImGui::Spacing();

  const float gap = S(12.f);
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float card_w = (avail_w - gap * 3.f) / 4.f;
  const float card_h = S(148.f);

  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 4; ++col) {
      const int voice = row * 4 + col;
      if (col) {
        ImGui::SameLine(0.f, gap);
      }
      DrawVoiceCard(app, voice, card_w, card_h);
    }
    ImGui::Spacing();
  }

  ImGui::Spacing();
  fw::ui::BeginSection("sample_help", "SIGNAL PATH", ImVec2(0, S(100.f)));
  ImGui::NewLine();
  ImGui::Spacing();
  MonoText("Attack on card (CDC)  ·  Body via UAC (card pitches, hungry mux)",
           kPalette.text, fw::theme::g_fonts.mono);
  ImGui::Spacing();
  MonoText("Prefill UAC then RS485 n0..n7  ·  vq 10 ms  ·  env / filter on card.",
           kPalette.text_dim, fs);
  ImGui::Spacing();
  MonoText("Wave: wav/raw SRC to 48 kHz. Folder: wN_head.i32 + wN_body.i16.",
           kPalette.text_dim, fs);
  fw::ui::EndSection();

  ImGui::EndChild();
}
