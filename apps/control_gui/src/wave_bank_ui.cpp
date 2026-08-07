#include "wave_bank_ui.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "wave_cdc.hpp"
#include "widgets.hpp"

#include "imgui.h"
#include "rs485/types.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using fw::theme::kPalette;

namespace
{

const char *SlotTitle(int i)
{
  static const char *k[] = {"Sine A4",  "Saw A3",   "Square E4", "Noise",
                            "Chirp",    "Sine C2",  "Pluck",     "Dyad C+E"};
  return (i >= 0 && i < 8) ? k[i] : "?";
}

ImVec4 StatusColor(WaveSlotUi ui)
{
  switch (ui) {
  case WaveSlotUi::Done:
    return kPalette.success;
  case WaveSlotUi::Failed:
    return kPalette.danger;
  case WaveSlotUi::Uploading:
  case WaveSlotUi::Queued:
    return kPalette.accent;
  case WaveSlotUi::Assigned:
    return kPalette.warning;
  default:
    return kPalette.text_dim;
  }
}

std::string Basename(const char *path)
{
  if (!path || !path[0]) {
    return {};
  }
  const char *slash = std::strrchr(path, '/');
  return slash ? std::string(slash + 1) : std::string(path);
}

void AssignFolder(App &app, const std::string &folder)
{
  if (folder.empty()) {
    return;
  }
  int hit = 0;
  try {
    for (int slot = 0; slot < 8; ++slot) {
      char prefix[8];
      std::snprintf(prefix, sizeof(prefix), "w%d", slot);
      std::string found;
      for (const auto &ent : fs::directory_iterator(folder)) {
        if (!ent.is_regular_file()) {
          continue;
        }
        const auto name = ent.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && name.size() >= 4 &&
            name.compare(name.size() - 4, 4, ".raw") == 0) {
          found = ent.path().string();
          break;
        }
      }
      if (!found.empty()) {
        app.waves.SetSlotPath(slot, found);
        ++hit;
      }
    }
  } catch (const std::exception &ex) {
    app.log.Push(std::string("err: folder ") + ex.what());
    return;
  }
  char msg[64];
  std::snprintf(msg, sizeof(msg), "ok: assigned %d / 8 from folder", hit);
  app.log.Push(msg);
}

void DrawMiniWave(const WaveSlotState &st, const ImVec2 &size)
{
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##waveviz", size);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, fw::theme::U32A(kPalette.bg, 0.85f), 4.f);
  dl->AddRect(p0, p1, fw::theme::U32A(kPalette.border, 0.7f), 4.f);

  if (!st.has_preview) {
    const char *msg = "no preview";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f,
                       p0.y + (size.y - ts.y) * 0.5f),
                fw::theme::U32(kPalette.text_dim), msg);
    return;
  }

  const float mid = p0.y + size.y * 0.5f;
  dl->AddLine(ImVec2(p0.x + 2.f, mid), ImVec2(p1.x - 2.f, mid),
              fw::theme::U32A(kPalette.border, 0.45f), 1.f);

  const int n = WaveSlotState::kPreviewN;
  ImVec2 pts[WaveSlotState::kPreviewN];
  for (int i = 0; i < n; ++i) {
    const float t = (n <= 1) ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
    const float y = mid - st.preview[i] * (size.y * 0.42f);
    pts[i] = ImVec2(p0.x + 3.f + t * (size.x - 6.f), y);
  }
  dl->AddPolyline(pts, n, fw::theme::U32(kPalette.accent), 0, 1.6f);
}

void DrawSlotCard(App &app, int slot, const WaveSlotState &st, float card_w,
                  float card_h)
{
  ImGui::PushID(slot);
  ImGui::BeginChild("slot", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetWindowPos();
  dl->AddRectFilled(origin, ImVec2(origin.x + card_w, origin.y + 3.f),
                    fw::theme::U32(StatusColor(st.ui)), 0.f);

  const float pad = 6.f;
  const float left_w = card_w * 0.48f - pad;
  const float right_w = card_w - left_w - pad * 3.f;
  const float body_h = card_h - 10.f;

  ImGui::SetCursorPos(ImVec2(pad, 8.f));
  ImGui::BeginChild("left", ImVec2(left_w, body_h), ImGuiChildFlags_None);

  ImGui::TextColored(kPalette.accent, "w%d", slot);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", SlotTitle(slot));

  const std::string base = Basename(st.path);
  if (base.empty()) {
    ImGui::TextColored(kPalette.text_dim, "No file");
  } else {
    ImGui::TextUnformatted(base.c_str());
  }
  ImGui::TextColored(StatusColor(st.ui), "%s",
                     st.status[0] ? st.status : "empty");

  if (st.ui == WaveSlotUi::Uploading || st.ui == WaveSlotUi::Queued) {
    fw::ui::ProgressBar("##p", st.progress, ImVec2(-1, 6));
  } else if (st.ui == WaveSlotUi::Done) {
    fw::ui::ProgressBar("##p", 1.f, ImVec2(-1, 6));
  }

  ImGui::BeginDisabled(app.waves.Busy());
  if (ImGui::SmallButton("Browse")) {
    const std::string picked = WaveCdc_PickRawFile();
    if (!picked.empty()) {
      app.waves.SetSlotPath(slot, picked);
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    app.waves.ClearSlot(slot);
  }
  ImGui::BeginDisabled(st.path[0] == '\0' || app.wave_cdc_path[0] == '\0');
  if (ImGui::SmallButton("Upload")) {
    app.waves.SetCdcPath(app.wave_cdc_path);
    app.waves.StartUpload(app.log, slot);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!app.bus.IsOpen() || app.play_mode != 1 ||
                       app.waves.Busy());
  if (ImGui::SmallButton("Play")) {
    app.wave_slot = slot;
    char cmd[48];
    std::snprintf(cmd, sizeof(cmd), "w%d %.0f", slot,
                  static_cast<double>(app.wave_rate));
    app.bus.QueueExec(rs485::Target::Channel, cmd);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Stop")) {
    char cmd[16];
    std::snprintf(cmd, sizeof(cmd), "w%d 0", slot);
    app.bus.QueueExec(rs485::Target::Channel, cmd);
  }
  ImGui::EndDisabled();

  ImGui::EndChild();

  ImGui::SameLine(0.f, pad);
  ImGui::BeginChild("right", ImVec2(right_w, body_h), ImGuiChildFlags_None);
  ImGui::TextDisabled("shape");
  DrawMiniWave(st, ImVec2(right_w - 4.f, body_h - 22.f));
  ImGui::EndChild();

  ImGui::EndChild();
  ImGui::PopID();
}

} // namespace

void DrawPlayModeStrip(App &app)
{
  ImGui::TextDisabled("PLAYBACK");
  ImGui::SameLine();
  const char *modes[] = {"Notes", "Wave"};
  int mode = app.play_mode;
  fw::ui::SegmentedControl("##pm", modes, 2, &mode);
  if (mode != app.play_mode) {
    app.play_mode = mode;
    if (app.bus.IsOpen()) {
      app.bus.QueueExec(rs485::Target::Channel,
                        app.play_mode == 1 ? "mode wave" : "mode notes");
    }
  }
}

void DrawWaveBankPage(App &app)
{
  std::array<WaveSlotState, 8> snap{};
  app.waves.Snapshot(snap);

  if (app.wave_cdc_path[0]) {
    app.waves.SetCdcPath(app.wave_cdc_path);
  }

  /* Toolbar sizes to content — never clip Load folder / Upload all. */
  ImGui::BeginChild("wave_toolbar", ImVec2(0, 0),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

  ImGui::TextColored(kPalette.accent, "WAVE BANK");
  ImGui::SameLine();
  ImGui::TextDisabled("  8 x 32 KiB  ·  CDC  ·  one-shot");
  ImGui::SameLine(ImGui::GetContentRegionAvail().x > 280.f
                      ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 200.f
                      : 0.f);
  DrawPlayModeStrip(app);

  ImGui::Spacing();
  if (!app.serial_ports.empty()) {
    std::vector<const char *> items;
    for (const auto &p : app.serial_ports) {
      items.push_back(p.c_str());
    }
    if (app.wave_cdc_port_index < 0 ||
        app.wave_cdc_port_index >= static_cast<int>(app.serial_ports.size())) {
      app.wave_cdc_port_index = 0;
      for (int i = 0; i < static_cast<int>(app.serial_ports.size()); ++i) {
        const auto &p = app.serial_ports[static_cast<size_t>(i)];
        if (p.find("usbmodem") != std::string::npos ||
            p.find("ACM") != std::string::npos) {
          app.wave_cdc_port_index = i;
          break;
        }
      }
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::Combo("CDC", &app.wave_cdc_port_index, items.data(),
                     static_cast<int>(items.size()))) {
      std::snprintf(app.wave_cdc_path, sizeof(app.wave_cdc_path), "%s",
                    app.serial_ports[static_cast<size_t>(app.wave_cdc_port_index)]
                        .c_str());
      app.waves.SetCdcPath(app.wave_cdc_path);
    }
    if (app.wave_cdc_path[0] == '\0') {
      std::snprintf(app.wave_cdc_path, sizeof(app.wave_cdc_path), "%s",
                    app.serial_ports[static_cast<size_t>(app.wave_cdc_port_index)]
                        .c_str());
      app.waves.SetCdcPath(app.wave_cdc_path);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##rate", &app.wave_rate, 1000.f, 96000.f, "rate %.0f");
  } else {
    ImGui::InputText("CDC path", app.wave_cdc_path, sizeof(app.wave_cdc_path));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##rate", &app.wave_rate, 1000.f, 96000.f, "rate %.0f");
  }

  const float btn_w =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.f) /
      4.f;
  ImGui::BeginDisabled(app.waves.Busy());
  if (fw::ui::GlowButton("Load folder", ImVec2(btn_w, 0))) {
    AssignFolder(app, WaveCdc_PickFolder());
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(app.wave_cdc_path[0] == '\0');
  if (fw::ui::GlowButton("Upload all", ImVec2(btn_w, 0))) {
    app.waves.SetCdcPath(app.wave_cdc_path);
    if (app.waves.StartUpload(app.log, -1) && app.bus.IsOpen()) {
      app.play_mode = 1;
      app.bus.QueueExec(rs485::Target::Channel, "mode wave");
    }
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!app.waves.Busy());
  if (fw::ui::GlowButton("Cancel", ImVec2(btn_w, 0), true)) {
    app.waves.Cancel();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!app.bus.IsOpen());
  if (fw::ui::GlowButton(app.play_mode == 1 ? "Mode: WAVE" : "Mode: NOTES",
                         ImVec2(btn_w, 0))) {
    app.play_mode = app.play_mode == 1 ? 0 : 1;
    app.bus.QueueExec(rs485::Target::Channel,
                      app.play_mode == 1 ? "mode wave" : "mode notes");
  }
  ImGui::EndDisabled();

  if (app.waves.Busy()) {
    ImGui::TextColored(kPalette.accent, "Uploading w%d", app.waves.CurrentSlot());
    fw::ui::ProgressBar("##all", app.waves.OverallProgress(), ImVec2(-1, 8));
  }
  ImGui::EndChild();

  ImGui::Spacing();

  /* Slot grid fills remaining height; scrolls only if window is tiny. */
  ImGui::BeginChild("wave_grid", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);

  const float gap = 8.f;
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float avail_h = ImGui::GetContentRegionAvail().y;
  const float card_w = (avail_w - gap) * 0.5f;
  /* Prefer fitting 4 rows without forcing min that overflows. */
  float card_h = (avail_h - gap * 3.f) * 0.25f;
  card_h = std::clamp(card_h, 96.f, 160.f);

  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 2; ++col) {
      const int slot = row * 2 + col;
      if (col) {
        ImGui::SameLine(0.f, gap);
      }
      DrawSlotCard(app, slot, snap[static_cast<size_t>(slot)], card_w, card_h);
    }
  }
  ImGui::EndChild();
}
