#include "sample_ui.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "cardlink/sample/client.hpp"
#include "cardlink/audio/sample_dry.hpp"

#include "imgui.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

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

bool CallSample(App &app, bool ok, const std::string &err, const char *ok_msg)
{
  if (!ok) {
    app.log.Push(err);
    app.PushToastErr(err);
    return false;
  }
  if (ok_msg != nullptr) {
    app.log.Push(ok_msg);
    app.PushToastOk(ok_msg);
  }
  return true;
}

bool ApplyWaveFile(App &app, int voice, const std::string &path)
{
  std::string cdc_err;
  if (!app.EnsureAttackCdc(cdc_err)) {
    app.log.Push(cdc_err);
    app.PushToastErr(cdc_err);
    return false;
  }
  std::string err;
  char msg[64];
  std::snprintf(msg, sizeof msg, "ok: wav w%u → card + BODY",
                static_cast<unsigned>(voice));
  return CallSample(app, app.samples.LoadWave(static_cast<uint8_t>(voice),
                                              path, err),
                    err, msg);
}

bool DirHasRawBank(const fs::path &dir)
{
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return false;
  }
  try {
    for (const auto &ent : fs::directory_iterator(dir)) {
      if (!ent.is_regular_file()) {
        continue;
      }
      const auto name = ent.path().filename().string();
      if (name.size() >= 6 && name[0] == 'w' &&
          name.compare(name.size() - 4, 4, ".raw") == 0) {
        return true;
      }
    }
  } catch (...) {
    return false;
  }
  return false;
}

/** Walk parents of `start` for `waves/` or `cmi_control/waves/` with w0_*.raw. */
std::string FindWavesNear(fs::path start)
{
  for (int i = 0; i < 10 && !start.empty(); ++i) {
    const fs::path candidates[] = {
        start / "waves",
        start / "cmi_control" / "waves",
    };
    for (const auto &cand : candidates) {
      if (DirHasRawBank(cand)) {
        return cand.string();
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
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buf), ec).parent_path().string();
  }
#elif defined(__linux__)
  std::error_code ec;
  const fs::path link = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    return link.parent_path().string();
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
  const std::string exe = ExecutableDir();
  if (!exe.empty()) {
    found = FindWavesNear(exe);
  }
  return found;
}

using ProgressFn = std::function<void(float, const char *)>;

void SetProgress(const ProgressFn &fn, float p, const char *status)
{
  if (fn) {
    fn(p, status);
  }
}

/** Load w0_*.raw … w255_*.raw. Holds CDC open for the whole bank. */
int LoadRawBank(App &app, const std::string &folder, std::string &result,
                const ProgressFn &on_progress)
{
  std::string cdc_err;
  SetProgress(on_progress, 0.02f, "CDC");
  if (!app.samples.BeginCdc(cdc_err)) {
    result = cdc_err;
    app.log.Push(cdc_err);
    return 0;
  }
  struct EndCdc {
    cardlink::sample::Client &c;
    ~EndCdc() { c.EndCdc(); }
  } end_cdc{app.samples};
  (void)end_cdc;

  const fs::path root(folder);
  constexpr int kWaves = static_cast<int>(cardlink::audio::kAttackWaves);
  int loaded = 0;
  for (int v = 0; v < kWaves; ++v) {
    char st[16];
    std::snprintf(st, sizeof st, "w%d", v);
    SetProgress(on_progress, (static_cast<float>(v) + 0.05f) /
                                 static_cast<float>(kWaves),
                st);

    std::string path;
    try {
      for (const auto &ent : fs::directory_iterator(root)) {
        if (!ent.is_regular_file()) {
          continue;
        }
        const auto name = ent.path().filename().string();
        int wid = -1;
        if (name.size() < 6 || name[0] != 'w' ||
            name.compare(name.size() - 4, 4, ".raw") != 0) {
          continue;
        }
        size_t i = 1;
        if (i >= name.size() || name[i] < '0' || name[i] > '9') {
          continue;
        }
        wid = 0;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
          wid = wid * 10 + (name[i] - '0');
          ++i;
        }
        if (i >= name.size() || name[i] != '_' || wid != v) {
          continue;
        }
        path = ent.path().string();
        break;
      }
    } catch (...) {
      path.clear();
    }
    if (path.empty()) {
      continue;
    }
    std::string err;
    if (!app.samples.LoadWave(static_cast<uint16_t>(v), path, err)) {
      result = err;
      app.log.Push(err);
      return loaded;
    }
    ++loaded;
  }

  if (loaded > 0) {
    const fs::path roots = root / "roots.txt";
    if (fs::exists(roots)) {
      SetProgress(on_progress, 0.92f, "roots");
      std::string roots_err;
      (void)app.samples.Mixer().LoadRootsFile(roots.string(), roots_err);
      for (int v = 0; v < kWaves; ++v) {
        const double hz =
            app.samples.Mixer().BodyRootHz(static_cast<uint16_t>(v));
        if (hz > 0.0) {
          std::string ignore;
          (void)app.samples.SetRootHz(static_cast<uint16_t>(v), hz, ignore);
        }
      }
    }
    char msg[72];
    std::snprintf(msg, sizeof msg, "ok: loaded %d/%d raw bank", loaded,
                  kWaves);
    result = msg;
    app.log.Push(msg);
  } else {
    result = "err: raw bank not found (w0_*.raw … w255_*.raw)";
    app.log.Push(result);
  }
  SetProgress(on_progress, 1.f, "done");
  return loaded;
}

void FinishSampleLoad(App &app)
{
  SampleLoadJob &job = app.sample_load;
  std::string msg;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(job.mu);
    if (!job.ready) {
      return;
    }
    msg = std::move(job.result);
    ok = job.ok;
    job.ready = false;
    job.status.clear();
  }
  if (job.worker.joinable()) {
    job.worker.join();
  }
  if (msg.empty()) {
    return;
  }
  if (ok) {
    app.PushToastOk(msg);
  } else {
    app.PushToastErr(msg);
  }
}

void StartRawBankLoad(App &app, const std::string &folder)
{
  SampleLoadJob &job = app.sample_load;
  if (job.busy.exchange(true)) {
    return;
  }
  if (job.worker.joinable()) {
    job.worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(job.mu);
    job.ready = false;
    job.ok = false;
    job.result.clear();
    job.status = "starting";
  }
  job.progress.store(0.f);

  job.worker = std::thread([&app, folder] {
    SampleLoadJob &job = app.sample_load;
    const auto on_progress = [&job](float p, const char *st) {
      job.progress.store(p);
      if (st != nullptr) {
        std::lock_guard<std::mutex> lock(job.mu);
        job.status = st;
      }
    };
    std::string result;
    (void)LoadRawBank(app, folder, result, on_progress);
    {
      std::lock_guard<std::mutex> lock(job.mu);
      job.ok = result.rfind("ok:", 0) == 0;
      job.result = std::move(result);
      job.ready = true;
    }
    job.busy.store(false);
  });
}

void DrainPending(App &app)
{
  if (app.sample_load.busy.load() || app.pending_sample_folder.empty()) {
    return;
  }
  const std::string pending = std::move(app.pending_sample_folder);
  app.pending_sample_folder.clear();

  if (pending.rfind("wave:", 0) != 0) {
    return;
  }
  const auto c1 = pending.find(':', 5);
  if (c1 == std::string::npos) {
    return;
  }
  const int voice = std::atoi(pending.c_str() + 5);
  ApplyWaveFile(app, voice, pending.substr(c1 + 1));
}

void NoteOn(App &app, int voice, double hz)
{
  (void)app.EnsureSampleStream();
  app.samples.NoteOn(static_cast<uint8_t>(voice), hz);
}

void NoteOff(App &app, int voice)
{
  app.samples.NoteOff(static_cast<uint8_t>(voice));
}

void BeginPickWave(App &app, int voice)
{
  if (app.file_dialog.Busy()) {
    app.PushToastErr("file dialog busy");
    return;
  }
  app.sample_file_pick = SampleFilePick::Wave;
  app.sample_file_voice = voice;
  if (!app.file_dialog.BeginPickFile()) {
    app.sample_file_pick = SampleFilePick::None;
    app.sample_file_voice = -1;
    app.PushToastErr("file dialog busy");
  }
}

void DrawVoiceCard(App &app, int voice, float card_w, float card_h)
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;
  const bool dialog_busy = app.file_dialog.Busy();
  const bool load_busy = app.sample_load.busy.load();
  const cardlink::sample::Slot *slot =
      load_busy ? nullptr : &app.samples.GetSlot(static_cast<uint8_t>(voice));

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
  if (load_busy || slot == nullptr) {
    MonoText("loading\u2026", kPalette.muted, fs);
  } else {
    MonoText(slot->label.empty() ? "empty" : slot->label.c_str(),
             slot->label.empty() ? kPalette.muted : kPalette.text, fs);
  }

  {
    char status[64];
    if (load_busy || slot == nullptr) {
      std::snprintf(status, sizeof status, "atk \u2014  ·  body \u2014");
      MonoText(status, kPalette.muted, fs);
    } else {
      std::snprintf(status, sizeof status, "atk %s  ·  body %s",
                    slot->head_on_card ? "card" : "\u2014",
                    slot->body_ready ? "BODY" : "\u2014");
      MonoText(status,
               (slot->head_on_card && slot->body_ready) ? kPalette.accent
                                                        : kPalette.muted,
               fs);
    }
  }

  {
    std::string bn;
    if (!load_busy && slot != nullptr) {
      bn = Basename(slot->head_path.empty() ? slot->body_path
                                            : slot->head_path);
    }
    ImGui::PushFont(fs);
    ImGui::PushTextWrapPos(card_w - S(16.f));
    ImGui::TextColored(kPalette.text_dim, "%s", bn.empty() ? "\u2014" : bn.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(dialog_busy || load_busy);
  if (fw::ui::ChipBtn("Wave", false, BtnKind::Neutral)) {
    BeginPickWave(app, voice);
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0.f, S(8.f));
  const bool can_play =
      !load_busy && slot != nullptr && (slot->head_on_card || slot->body_ready);
  ImGui::BeginDisabled(!can_play);
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
  FinishSampleLoad(app);
  DrainPending(app);
  /* Keep CDC list fresh — card paths change when USB re-enumerates. */
  if (!app.sample_load.busy.load() && (ImGui::GetFrameCount() % 120) == 0) {
    app.RefreshPortLists();
  }

  ImFont *fs = fw::theme::g_fonts.mono_small;
  const bool stream_on = app.sample_bulk && app.sample_bulk->Running();
  const bool bus_ok = app.bus.IsOpen() && !app.bus.BusFault();
  const bool cdc_ok = app.attack_cdc_path[0] != '\0';
  const bool dialog_busy = app.file_dialog.Busy();
  const bool load_busy = app.sample_load.busy.load();

  // ── Top bar: CDC + BODY
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
          cdc_ok ? app.attack_cdc_path : "\u2014 select \u2014";
      ImGui::PushFont(fs);
      ImGui::BeginDisabled(load_busy);
      if (ImGui::BeginCombo("##sample_cdc", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(app.serial_ports.size()); ++i) {
          const auto &p = app.serial_ports[static_cast<std::size_t>(i)];
          if (ImGui::Selectable(p.c_str(), p == app.attack_cdc_path)) {
            app.attack_cdc_port_index = i;
            std::snprintf(app.attack_cdc_path, sizeof(app.attack_cdc_path), "%s",
                          p.c_str());
            app.samples.SetCdcPath(app.attack_cdc_path);
            app.MarkSettingsDirty();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      ImGui::PopFont();
    } else {
      ImGui::PushFont(fs);
      ImGui::BeginDisabled(load_busy);
      if (ImGui::InputTextWithHint("##sample_cdc_path", "CDC path\u2026",
                                   app.attack_cdc_path,
                                   sizeof(app.attack_cdc_path))) {
        app.samples.SetCdcPath(app.attack_cdc_path);
        app.MarkSettingsDirty();
      }
      ImGui::EndDisabled();
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
    fw::ui::StatusDot(3.f, stream_on ? kPalette.accent : kPalette.muted, stream_on);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText(stream_on ? "BODY ON" : "BODY OFF",
             stream_on ? kPalette.accent : kPalette.text_dim, fs);

    ImGui::SameLine(0.f, S(12.f));
    ImGui::SetCursorPosY(mid_y);
    if (!stream_on) {
      if (fw::ui::Btn("Start BODY", ImVec2(0, S(22.f)), BtnKind::Primary)) {
        if (!app.EnsureSampleStream()) {
          /* EnsureSampleStream already logs. */
        } else {
          app.PushToastOk("BODY stream open");
        }
      }
    } else if (fw::ui::Btn("Stop BODY", ImVec2(0, S(22.f)), BtnKind::Neutral)) {
      app.sample_bulk->Stop();
    }

    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText("51 kHz UAC · 48 kHz BODY", kPalette.muted, fs);
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
    MonoText("wav/raw per wave_id 0..255 · SDK splits attack + body", kPalette.muted,
             fs);

    ImGui::Spacing();
    ImGui::BeginDisabled(dialog_busy || !cdc_ok || load_busy);
    if (fw::ui::Btn(load_busy ? "Loading\u2026" : "Load raw bank",
                    ImVec2(0, S(24.f)), BtnKind::Primary)) {
      const std::string waves = FindWavesDir();
      if (waves.empty()) {
        app.PushToastErr(
            "waves/ not found (need w0_*.raw under cmi_control/waves)");
      } else {
        std::string cdc_err;
        if (!app.EnsureAttackCdc(cdc_err)) {
          app.log.Push(cdc_err);
          app.PushToastErr(cdc_err);
        } else {
          StartRawBankLoad(app, waves);
        }
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (!cdc_ok) {
        ImGui::SetTooltip("Select Channel Card CDC port first");
      } else if (load_busy) {
        ImGui::SetTooltip("Uploading attack heads over CDC");
      } else {
        ImGui::SetTooltip("Load w0_*.raw \u2026 w255_*.raw from waves/");
      }
    }
    if (load_busy) {
      ImGui::SameLine(0.f, S(12.f));
      std::string st;
      {
        std::lock_guard<std::mutex> lock(app.sample_load.mu);
        st = app.sample_load.status;
      }
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(10.f));
      fw::ui::ProgressBar("raw_bank", app.sample_load.progress.load(),
                          ImVec2(S(140.f), S(4.f)));
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6.f));
      MonoText(st.c_str(), kPalette.accent, fs);
    }

    ImGui::SameLine(0.f, S(12.f));
    ImGui::BeginDisabled(!bus_ok);
    if (fw::ui::Btn("8-note chord", ImVec2(0, S(24.f)), BtnKind::Primary)) {
      std::array<cardlink::sample::NoteRequest, kVoices> notes{};
      for (int v = 0; v < kVoices; ++v) {
        notes[static_cast<size_t>(v)] = cardlink::sample::NoteRequest{
            static_cast<uint8_t>(v), kChordHz[v], 0xFFFFu};
      }
      (void)app.EnsureSampleStream();
      (void)app.samples.NoteOnBatch(notes.data(), notes.size());
    }
    ImGui::SameLine(0.f, S(6.f));
    if (fw::ui::Btn("All off", ImVec2(0, S(24.f)), BtnKind::Neutral)) {
      app.samples.AllNotesOff();
      app.bus.RequestSilence();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.f, S(16.f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5.f));
    if (!bus_ok) {
      MonoText("RS485 offline", kPalette.warning, fs);
    } else if (!stream_on) {
      MonoText("start BODY for sustain", kPalette.muted, fs);
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
  const float card_h = S(120.f);

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
  MonoText("Attack on card (CDC)  ·  Body via UAC2 int16 (card pitches)",
           kPalette.text, fw::theme::g_fonts.mono);
  ImGui::Spacing();
  MonoText("RS485 note + concurrent BODY prefill  ·  env / filter on card.",
           kPalette.text_dim, fs);
  ImGui::Spacing();
  MonoText("Pass wav/raw per wave_id 0..255; SDK splits attack (CDC) + body (USB).",
           kPalette.text_dim, fs);
  fw::ui::EndSection();

  ImGui::EndChild();
}
