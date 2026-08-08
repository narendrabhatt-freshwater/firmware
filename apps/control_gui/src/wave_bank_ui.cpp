#include "wave_bank_ui.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "wave_cdc.hpp"
#include "widgets.hpp"

#include "protocol/channel.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using fw::theme::kPalette;
using fw::theme::S;
using fw::theme::S2;
using fw::ui::BtnKind;

namespace
{

ImVec4 StatusColor(WaveSlotUi ui)
{
  switch (ui) {
  case WaveSlotUi::Done:
    return kPalette.accent;
  case WaveSlotUi::Assigned:
    return kPalette.text_dim;
  case WaveSlotUi::Failed:
    return kPalette.danger;
  case WaveSlotUi::Uploading:
  case WaveSlotUi::Queued:
    return kPalette.warning;
  default:
    return kPalette.muted;
  }
}

const char *StatusLabel(WaveSlotUi ui)
{
  switch (ui) {
  case WaveSlotUi::Done:
    return "DONE";
  case WaveSlotUi::Failed:
    return "FAILED";
  case WaveSlotUi::Uploading:
    return "UPLOADING";
  case WaveSlotUi::Queued:
    return "QUEUED";
  case WaveSlotUi::Assigned:
    return "ASSIGNED";
  default:
    return "EMPTY";
  }
}

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
  if (hit > 0) {
    std::snprintf(msg, sizeof(msg), "ok: auto-assigned %d/8 wave slots", hit);
  } else {
    std::snprintf(msg, sizeof(msg),
                  "err: auto-assign found no w0…w7 .raw files");
  }
  app.log.Push(msg);
}

/** Waveform thumbnail (design 120×32 svg): preview polyline or em-dash. */
void DrawWaveThumb(const WaveSlotState &st, const ImVec2 &size)
{
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##waveviz", size);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, fw::theme::U32(kPalette.bg_alt), S(2.f));
  dl->AddRect(p0, p1, fw::theme::U32(kPalette.border), S(2.f));

  if (st.ui == WaveSlotUi::Empty || !st.has_preview) {
    ImFont *fs = fw::theme::g_fonts.caps;
    const char *msg = "\u2014";
    const ImVec2 ts = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, msg);
    dl->AddText(fs, fs->FontSize,
                ImVec2(p0.x + (size.x - ts.x) * 0.5f,
                       p0.y + (size.y - ts.y) * 0.5f),
                fw::theme::U32(kPalette.muted), msg);
    return;
  }

  const float mid = p0.y + size.y * 0.5f;
  dl->AddLine(ImVec2(p0.x + 2.f, mid), ImVec2(p1.x - 2.f, mid),
              fw::theme::U32A(kPalette.accent, 0.06f), 1.f);
  const ImVec4 col = StatusColor(st.ui);
  const float alpha = (st.ui == WaveSlotUi::Done) ? 1.f : 0.6f;
  const int n = WaveSlotState::kPreviewN;
  ImVec2 pts[WaveSlotState::kPreviewN];
  for (int i = 0; i < n; ++i) {
    const float t =
        (n <= 1) ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
    const float y = mid - st.preview[i] * (size.y * 0.38f);
    pts[i] = ImVec2(p0.x + S(3.f) + t * (size.x - S(6.f)), y);
  }
  dl->AddPolyline(pts, n, fw::theme::U32A(col, 0.25f * alpha), 0, 3.f);
  dl->AddPolyline(pts, n, fw::theme::U32A(col, alpha), 0, 1.2f);
}

void DrawSlotCard(App &app, int slot, const WaveSlotState &st, float card_w,
                  float card_h)
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;
  const bool bus_online = app.bus.IsOpen() && !app.bus.BusFault();
  const bool empty = (st.ui == WaveSlotUi::Empty);
  const bool uploading =
      (st.ui == WaveSlotUi::Uploading || st.ui == WaveSlotUi::Queued);

  ImGui::PushID(slot);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
  ImGui::PushStyleColor(
      ImGuiCol_Border,
      uploading ? ImVec4(kPalette.warning.x, kPalette.warning.y,
                         kPalette.warning.z, 0.30f)
                : kPalette.border);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(10.f, 8.f));
  ImGui::BeginChild("slot", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  ImGui::PopStyleVar();

  // Header: label + state
  {
    char lbl[4];
    std::snprintf(lbl, sizeof(lbl), "w%d", slot);
    MonoText(lbl, empty ? kPalette.muted : kPalette.accent, fm);
    const char *state = StatusLabel(st.ui);
    const float sw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, state).x;
    ImGui::SameLine(card_w - sw - S(12.f));
    ImGui::SetCursorPosY(S(10.f));
    MonoText(state, StatusColor(st.ui), fs);
  }

  // Waveform thumb
  DrawWaveThumb(st, ImVec2(card_w - S(20.f), S(32.f)));

  // Filename
  {
    const std::string base = Basename(st.path);
    if (!base.empty()) {
      ImGui::PushFont(fs);
      ImGui::PushTextWrapPos(card_w - S(10.f));
      ImGui::TextColored(kPalette.text_dim, "%s", base.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
    } else {
      MonoText("Drop .raw file or browse", kPalette.muted, fs);
    }
  }

  // Progress
  if (st.ui == WaveSlotUi::Uploading) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = card_w - S(20.f);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + S(3.f)),
                      fw::theme::U32(kPalette.border), S(1.5f));
    dl->AddRectFilled(
        p0,
        ImVec2(p0.x + w * std::clamp(st.progress, 0.f, 1.f), p0.y + S(3.f)),
        fw::theme::U32(kPalette.warning), S(1.5f));
    ImGui::Dummy(ImVec2(w, S(5.f)));
  } else if (st.ui == WaveSlotUi::Done) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = card_w - S(20.f);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + S(3.f)),
                      fw::theme::U32(kPalette.accent), S(1.5f));
    ImGui::Dummy(ImVec2(w, S(5.f)));
  } else {
    ImGui::Dummy(ImVec2(0, S(5.f)));
  }

  // Rate (per-slot playback rate, Hz)
  if (!empty) {
    MonoText("Rate:", kPalette.muted, fs);
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetNextItemWidth(S(70.f));
    float rate = app.wave_slot_rate[static_cast<std::size_t>(slot)];
    ImGui::PushFont(fs);
    if (ImGui::InputFloat("##rate", &rate, 0.f, 0.f, "%.0f")) {
      app.wave_slot_rate[static_cast<std::size_t>(slot)] =
          std::clamp(rate, 1000.f, 96000.f);
    }
    ImGui::PopFont();
    ImGui::SameLine(0.f, S(6.f));
    MonoText("Hz", kPalette.muted, fs);
  } else {
    ImGui::Dummy(ImVec2(0, S(20.f)));
  }

  // Actions
  {
    ImGui::BeginDisabled(app.waves.Busy() || app.file_dialog.Busy());
    if (fw::ui::ChipBtn("Browse", false, BtnKind::Neutral)) {
      app.file_dialog_kind = AsyncFileDialog::Kind::File;
      app.file_dialog_slot = slot;
      if (!app.file_dialog.BeginPickFile()) {
        app.PushToastErr("file dialog busy");
      }
    }
    ImGui::EndDisabled();
    if (!empty) {
      ImGui::SameLine(0.f, S(4.f));
      ImGui::BeginDisabled(uploading);
      if (fw::ui::ChipBtn("Clear", false, BtnKind::Danger)) {
        app.waves.ClearSlot(slot);
      }
      ImGui::SameLine(0.f, S(4.f));
      ImGui::BeginDisabled(app.wave_cdc_path[0] == '\0');
      if (fw::ui::ChipBtn("Upload", false, BtnKind::Primary)) {
        app.waves.SetCdcPath(app.wave_cdc_path);
        app.waves.StartUpload(app.log, slot);
      }
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      if (bus_online && st.ui == WaveSlotUi::Done) {
        ImGui::SameLine(0.f, S(4.f));
        if (fw::ui::ChipBtn("\u25B6 Play", false, BtnKind::Primary)) {
          const double rate = static_cast<double>(
              app.wave_slot_rate[static_cast<std::size_t>(slot)]);
          app.log.Push(LogKind::Tx,
                       "tx wave play w" + std::to_string(slot));
          app.bus.QueuePlayWave(static_cast<uint8_t>(slot), rate);
        }
        ImGui::SameLine(0.f, S(4.f));
        if (fw::ui::ChipBtn("\u25A0", false, BtnKind::Neutral)) {
          app.log.Push(LogKind::Tx,
                       "tx wave stop w" + std::to_string(slot));
          app.bus.QueueStopWave(static_cast<uint8_t>(slot));
        }
      }
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopID();
}

} // namespace

void DrawWaveBankPage(App &app)
{
  ImFont *fs = fw::theme::g_fonts.mono_small;

  std::array<WaveSlotState, 8> snap{};
  app.waves.Snapshot(snap);

  if (!app.pending_drops.empty()) {
    for (const auto &p : app.pending_drops) {
      if (p.rfind("folder:", 0) == 0) {
        AssignFolder(app, p.substr(7));
      } else {
        int dest = 0;
        for (int s = 0; s < 8; ++s) {
          if (snap[static_cast<std::size_t>(s)].path[0] == '\0') {
            dest = s;
            break;
          }
        }
        app.waves.SetSlotPath(dest, p);
        app.PushToastOk(std::string("assigned drop \u2192 w") +
                        std::to_string(dest));
      }
    }
    app.pending_drops.clear();
    app.waves.Snapshot(snap);
  }

  if (app.wave_cdc_path[0]) {
    app.waves.SetCdcPath(app.wave_cdc_path);
  }

  const bool uploading = app.waves.Busy();
  int assigned = 0;
  for (const auto &s : snap) {
    if (s.ui == WaveSlotUi::Assigned || s.ui == WaveSlotUi::Failed) {
      ++assigned;
    }
  }

  // ── Top bar
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.f), 0.f));
  ImGui::BeginChild("waves_bar", ImVec2(0, S(44.f)), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x, wp.y + wsz.y - 1.f),
                ImVec2(wp.x + wsz.x, wp.y + wsz.y - 1.f),
                fw::theme::U32(kPalette.border));

    const float mid_y = S(11.f);
    ImGui::SetCursorPos(ImVec2(S(16.f), mid_y + S(4.f)));
    fw::ui::StatusDot(3.f, app.wave_cdc_path[0] ? kPalette.accent
                                                : kPalette.muted,
                      app.wave_cdc_path[0] != '\0');
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(5.f));
    MonoText("WAVE CDC PORT", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y);
    ImGui::SetNextItemWidth(S(180.f));
    if (!app.serial_ports.empty()) {
      std::string preview = app.wave_cdc_path[0]
                                ? app.wave_cdc_path
                                : "\u2014 select \u2014";
      ImGui::PushFont(fs);
      if (ImGui::BeginCombo("##cdc", preview.c_str())) {
        for (int i = 0; i < static_cast<int>(app.serial_ports.size()); ++i) {
          const auto &p = app.serial_ports[static_cast<std::size_t>(i)];
          if (ImGui::Selectable(p.c_str(), p == app.wave_cdc_path)) {
            app.wave_cdc_port_index = i;
            std::snprintf(app.wave_cdc_path, sizeof(app.wave_cdc_path), "%s",
                          p.c_str());
            app.waves.SetCdcPath(app.wave_cdc_path);
            app.MarkSettingsDirty();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopFont();
    } else {
      ImGui::PushFont(fs);
      if (ImGui::InputTextWithHint("##cdcpath", "CDC path…",
                                   app.wave_cdc_path,
                                   sizeof(app.wave_cdc_path))) {
        app.waves.SetCdcPath(app.wave_cdc_path);
        app.MarkSettingsDirty();
      }
      ImGui::PopFont();
    }

    // divider
    ImGui::SameLine(0.f, S(10.f));
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(p.x, wp.y + S(12.f)), ImVec2(p.x, wp.y + S(28.f)),
                  fw::theme::U32(kPalette.border));
      ImGui::Dummy(ImVec2(1.f, 0.f));
    }

    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(mid_y);
    ImGui::BeginDisabled(uploading || app.file_dialog.Busy());
    if (fw::ui::Btn("\u229E Assign to All", ImVec2(0, S(22.f)),
                    BtnKind::Neutral)) {
      app.file_dialog_kind = AsyncFileDialog::Kind::File;
      app.file_dialog_slot = -2;
      if (!app.file_dialog.BeginPickFile()) {
        app.PushToastErr("file dialog busy");
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Pick one file and assign it to all 8 slots");
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(mid_y);
    if (fw::ui::Btn("\u229F Auto-assign Folder", ImVec2(0, S(22.f)),
                    BtnKind::Neutral)) {
      app.file_dialog_kind = AsyncFileDialog::Kind::Folder;
      app.file_dialog_slot = -1;
      if (!app.file_dialog.BeginPickFolder()) {
        app.PushToastErr("file dialog busy");
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Pick a folder — assigns by w0…w7 name prefix");
    }
    ImGui::EndDisabled();

    // divider
    ImGui::SameLine(0.f, S(10.f));
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddLine(ImVec2(p.x, wp.y + S(12.f)), ImVec2(p.x, wp.y + S(28.f)),
                  fw::theme::U32(kPalette.border));
      ImGui::Dummy(ImVec2(1.f, 0.f));
    }

    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(mid_y);
    {
      char up[32];
      std::snprintf(up, sizeof(up), "\u2191 Upload All (%d)", assigned);
      ImGui::BeginDisabled(assigned == 0 || uploading ||
                           app.wave_cdc_path[0] == '\0');
      if (fw::ui::Btn(up, ImVec2(0, S(22.f)), BtnKind::Primary)) {
        app.waves.SetCdcPath(app.wave_cdc_path);
        if (app.waves.StartUpload(app.log, -1) && app.bus.IsOpen()) {
          app.play_mode = 1;
          app.bus.QueueMode(protocol::PlayMode::Wave);
        }
      }
      ImGui::EndDisabled();
    }
    if (uploading) {
      ImGui::SameLine(0.f, S(6.f));
      ImGui::SetCursorPosY(mid_y);
      if (fw::ui::Btn("\u2715 Cancel", ImVec2(0, S(22.f)),
                      BtnKind::Danger)) {
        app.waves.Cancel();
      }
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(mid_y + S(5.f));
      char pct[16];
      std::snprintf(pct, sizeof(pct), "%d%%",
                    static_cast<int>(app.waves.OverallProgress() * 100.f +
                                     0.5f));
      MonoText(pct, kPalette.warning, fs);
    }

    // Right: mode warning
    if (app.play_mode == 0) {
      const char *warn_txt = "\u26A0 Mode is Notes \u2014";
      const char *link_txt = "switch in Tone";
      ImFont *cf = fw::theme::g_fonts.caps;
      const float w1 = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, warn_txt).x;
      const float w2 = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, link_txt).x;
      ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(10.f),
                               wsz.x - w1 - w2 - S(24.f)));
      ImGui::SetCursorPosY(mid_y + S(4.f));
      MonoText(warn_txt, kPalette.warning, cf);
      ImGui::SameLine(0.f, S(4.f));
      ImGui::SetCursorPosY(mid_y + S(4.f));
      ImGui::PushFont(cf);
      ImGui::TextColored(kPalette.accent, "%s", link_txt);
      const ImVec2 mn = ImGui::GetItemRectMin();
      const ImVec2 mx = ImGui::GetItemRectMax();
      dl->AddLine(ImVec2(mn.x, mx.y - 1.f), ImVec2(mx.x, mx.y - 1.f),
                  fw::theme::U32A(kPalette.accent, 0.7f));
      ImGui::PopFont();
      if (ImGui::IsItemClicked()) {
        app.view = GuiView::Tone;
        app.MarkSettingsDirty();
      }
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // ── CDC note strip
  {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(kPalette.warning.x, kPalette.warning.y,
                                 kPalette.warning.z, 0.04f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.f), 0.f));
    ImGui::BeginChild("waves_note", ImVec2(0, S(40.f)), ImGuiChildFlags_None);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x, wp.y + wsz.y - 1.f),
                ImVec2(wp.x + wsz.x, wp.y + wsz.y - 1.f),
                fw::theme::U32(kPalette.border));
    ImGui::SetCursorPos(ImVec2(S(16.f), S(12.f)));
    MonoText("\u229F", kPalette.warning, fs);
    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(S(12.f));
    ImGui::PushFont(fs);
    ImGui::TextColored(kPalette.text_dim,
                       "Wave upload uses a separate USB CDC port from the "
                       "RS485 control bus. Select the Channel USB modem port "
                       "above — not the RS485 port in Setup.");
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }

  // ── 4-column slot grid
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(12.f, 12.f));
  ImGui::BeginChild("wave_grid", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGui::PopStyleVar();
  const float gap = S(12.f);
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float card_w = (avail_w - gap * 3.f) / 4.f;
  const float card_h = S(174.f);

  for (int row = 0; row < 2; ++row) {
    for (int col = 0; col < 4; ++col) {
      const int slot = row * 4 + col;
      if (col) {
        ImGui::SameLine(0.f, gap);
      }
      DrawSlotCard(app, slot, snap[static_cast<std::size_t>(slot)], card_w,
                   card_h);
    }
    ImGui::Spacing();
  }
  ImGui::EndChild();
}
