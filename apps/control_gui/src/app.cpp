#include "app.hpp"

#include "anim.hpp"
#include "env_editor.hpp"
#include "note_event.hpp"
#include "host_io/serial_port.hpp"
#include "protocol/types.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "wave_bank_ui.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
using fw::theme::kPalette;
using fw::theme::S;
using fw::theme::Metrics;

const char *QueueResultMsg(BusQueueResult r)
{
  switch (r) {
  case BusQueueResult::Closed:
    return "bus offline — connect in Setup";
  case BusQueueResult::Halted:
    return "bus fault — press Recover";
  case BusQueueResult::Connecting:
    return "bus still connecting…";
  default:
    return nullptr;
  }
}
} // namespace

void App::MarkSettingsDirty()
{
  settings_dirty = true;
  settings_save_countdown = 0.8f;
}

void App::PushToastOk(const std::string &msg)
{
  toasts.Push(fw::ui::ToastKind::Success, msg);
}

void App::PushToastErr(const std::string &msg)
{
  toasts.Push(fw::ui::ToastKind::Error, msg);
}

void App::NotifyEnqueue(bool ok, const char *what)
{
  if (ok) {
    return;
  }
  PushToastErr(what ? what : "command dropped");
}

void App::RefreshPortLists()
{
  serial_ports = host_io::SerialPort::ListPorts();
  midi_ports = midi_host::MidiInput::ListPorts();
  if (serial_path_buf[0] == '\0' && !serial_ports.empty()) {
    std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                  serial_ports.front().c_str());
  }
}

bool App::EnsureAudio()
{
  if (audio_open) {
    return true;
  }
  try {
    audio = std::make_unique<midi_host::AudioEngine>();
    audio->Start(48000);
    audio_open = true;
    log.Push(std::string("ok: speakers ") + audio->DeviceName());
    return true;
  } catch (const std::exception &ex) {
    log.Push(std::string("err: audio ") + ex.what());
    PushToastErr(std::string("audio: ") + ex.what());
    audio.reset();
    audio_open = false;
    return false;
  }
}

void App::ShutdownAudio()
{
  if (audio) {
    audio->Stop();
    audio.reset();
  }
  audio_open = false;
}

bool App::ConnectMidi()
{
  DisconnectMidi();
  try {
    std::optional<unsigned> idx;
    if (midi_port_index >= 0) {
      idx = static_cast<unsigned>(midi_port_index);
    }
    midi.Open(idx);
    midi_open = true;
    log.Push(std::string("ok: MIDI ") + midi.PortName());
    PushToastOk(std::string("MIDI ") + midi.PortName());
    MarkSettingsDirty();
    return true;
  } catch (const std::exception &ex) {
    log.Push(std::string("err: MIDI ") + ex.what());
    PushToastErr(std::string("MIDI: ") + ex.what());
    midi_open = false;
    return false;
  }
}

void App::DisconnectMidi()
{
  if (midi_open) {
    midi.Close();
    midi_open = false;
    log.Push("ok: MIDI closed");
  }
}

bool App::ConnectBus()
{
  RequestConnectBus();
  return true;
}

void App::RequestConnectBus()
{
  const std::string path = serial_path_buf;
  if (path.empty()) {
    log.Push("err: no RS485 path");
    PushToastErr("no RS485 path");
    return;
  }
  if (bus.IsConnecting()) {
    PushToastErr("bus still connecting…");
    return;
  }
  bus.RequestOpen(path, baud, static_cast<uint32_t>(gain_db), log);
  MarkSettingsDirty();
}

void App::DisconnectBus()
{
  if (bus.IsOpen() || bus.IsConnecting()) {
    bus.Close(log);
  }
}

void App::ApplyBankEvents(const std::vector<midi_host::BankEvent> &events)
{
  const bool want_speakers =
      (out_mode == OutMode::Speakers || out_mode == OutMode::Both);
  const bool want_card =
      (out_mode == OutMode::Card || out_mode == OutMode::Both);

  if (want_speakers) {
    EnsureAudio();
  }

  for (const auto &ev : events) {
    if (want_speakers && audio) {
      audio->ApplyBankEvent(ev);
    }
  }

  if (want_card && bus.IsOpen()) {
    bus.PublishBank(bank);
  }
}

void App::AllNotesOff()
{
  const auto evs = bank.AllOff();
  ApplyBankEvents(evs);
  if (bus.IsOpen()) {
    bus.RequestSilence();
  }
}

void App::HandleKeyboardPiano()
{
  ImGuiIO &io = ImGui::GetIO();
  if (io.WantTextInput || view != GuiView::Perform) {
    return;
  }

  // White: A S D F G H J K ; black: W E T Y U ; octave Z/X
  struct Map
  {
    ImGuiKey key;
    int semitone; // relative to C of current octave
  };
  static const Map kMap[] = {
      {ImGuiKey_A, 0},  {ImGuiKey_W, 1},  {ImGuiKey_S, 2},  {ImGuiKey_E, 3},
      {ImGuiKey_D, 4},  {ImGuiKey_F, 5},  {ImGuiKey_T, 6},  {ImGuiKey_G, 7},
      {ImGuiKey_Y, 8},  {ImGuiKey_H, 9},  {ImGuiKey_U, 10}, {ImGuiKey_J, 11},
      {ImGuiKey_K, 12},
  };
  if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
    piano_octave = std::max(piano_octave - 1, -2);
    MarkSettingsDirty();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
    piano_octave = std::min(piano_octave + 1, 4);
    MarkSettingsDirty();
  }
  const int base = 60 + piano_octave * 12; // C4 ± octave
  for (const auto &m : kMap) {
    const int note = base + m.semitone;
    if (note < 0 || note > 127) {
      continue;
    }
    if (ImGui::IsKeyPressed(m.key, false)) {
      ApplyBankEvents(bank.NoteOn(static_cast<uint8_t>(note)));
    }
    if (ImGui::IsKeyReleased(m.key)) {
      ApplyBankEvents(bank.NoteOff(static_cast<uint8_t>(note)));
    }
  }
}

void App::DrawDisconnectedHint(const char *action)
{
  fw::ui::EmptyState("Not connected", action);
  if (fw::ui::GlowButton("Open Setup", ImVec2(S(160.f), S(32.f)))) {
    view = GuiView::Setup;
    MarkSettingsDirty();
  }
}

void App::DrawBusFaultBanner()
{
  if (!bus.IsOpen() || !bus.BusFault()) {
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        ImVec4(0.35f, 0.08f, 0.08f, 0.95f));
  ImGui::BeginChild("fault_banner", ImVec2(0, S(36.f)), ImGuiChildFlags_Borders);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kPalette.danger, "BUS FAULT — note TX halted");
  ImGui::SameLine();
  if (fw::ui::GlowButton("Recover", ImVec2(S(100.f), S(26.f)))) {
    bus.RequestRecover(log);
    PushToastOk("recover queued");
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::DrawStatusBar()
{
  const float h = S(Metrics::StatusBarH);
  ImGui::BeginChild("statusbar", ImVec2(0, h), ImGuiChildFlags_None);
  const char *bus_lbl = bus.IsConnecting() ? "connecting…"
                        : bus.IsOpen()     ? (bus.BusFault() ? "FAULT" : "online")
                                           : "offline";
  ImGui::TextDisabled("BUS");
  ImGui::SameLine();
  ImGui::TextColored(bus.BusFault()     ? kPalette.danger
                     : bus.IsOpen()     ? kPalette.success
                     : bus.IsConnecting() ? kPalette.warning
                                          : kPalette.text_dim,
                     "%s", bus_lbl);
  ImGui::SameLine(0.f, S(Metrics::SpaceL));
  ImGui::TextDisabled("Q");
  ImGui::SameLine();
  ImGui::Text("%zu", bus.QueueDepth());
  ImGui::SameLine(0.f, S(Metrics::SpaceL));
  ImGui::TextDisabled("MIDI");
  ImGui::SameLine();
  if (midi_activity) {
    ImGui::TextColored(kPalette.accent, "●");
  } else {
    ImGui::TextDisabled("○");
  }
  ImGui::SameLine(0.f, S(Metrics::SpaceL));
  ImGui::TextDisabled("to %u / err %u", bus.TimeoutCount(), bus.ErrCount());
  ImGui::SameLine();
  const float right = ImGui::GetContentRegionAvail().x;
  const char *hint = "Space silence · ? shortcuts · 1–6 views";
  const float hw = ImGui::CalcTextSize(hint).x;
  ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.f, right - hw));
  ImGui::TextDisabled("%s", hint);
  ImGui::EndChild();
}

void App::Tick()
{
  const float dt = ImGui::GetIO().DeltaTime;
  toasts.Tick(dt);

  if (settings_dirty) {
    settings_save_countdown -= dt;
    if (settings_save_countdown <= 0.f) {
      fw::settings::Save(*this);
      settings_dirty = false;
    }
  }

  if (midi_open) {
    std::vector<midi_host::NoteEvent> notes;
    midi.Poll(notes);
    if (!notes.empty()) {
      midi_activity = true;
      midi_activity_timer = 0.18f;
    }
    for (const auto &n : notes) {
      std::vector<midi_host::BankEvent> evs;
      if (n.action == midi_host::NoteAction::On) {
        evs = bank.NoteOn(n.key);
      } else if (n.action == midi_host::NoteAction::AllOff) {
        AllNotesOff();
        continue;
      } else {
        evs = bank.NoteOff(n.key);
      }
      ApplyBankEvents(evs);
    }
  }
  if (midi_activity_timer > 0.f) {
    midi_activity_timer -= dt;
    if (midi_activity_timer <= 0.f) {
      midi_activity = false;
    }
  }

  HandleKeyboardPiano();

  preview.SetVoices(bank);
  preview.Render(48000.f);

  const float peak = preview.Peak();
  if (peak >= peak_hold) {
    peak_hold = peak;
    peak_hold_timer = 1.2f;
  } else {
    peak_hold_timer -= dt;
    if (peak_hold_timer <= 0.f) {
      peak_hold = std::max(0.f, peak_hold - dt * 0.35f);
    }
  }

  log.Snapshot(log_view);

  // Drain async file dialogs (Waves page sets kind/slot before Begin*).
  if (!file_dialog.Busy()) {
    std::string path;
    if (file_dialog.TakeResult(path) && !path.empty()) {
      if (file_dialog_kind == AsyncFileDialog::Kind::Folder) {
        // Handled in wave_bank_ui via pending path — store for Waves.
        pending_drops.clear();
        pending_drops.push_back(std::string("folder:") + path);
      } else if (file_dialog_slot >= 0 && file_dialog_slot < 8) {
        waves.SetSlotPath(file_dialog_slot, path);
      } else if (file_dialog_slot == -2) {
        for (int s = 0; s < 8; ++s) {
          waves.SetSlotPath(s, path);
        }
      }
      file_dialog_kind = AsyncFileDialog::Kind::Idle;
      file_dialog_slot = -1;
    }
  }
}

void App::DrawSetup()
{
  const float gap = S(Metrics::SpaceM);
  const float card_w = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
  const float card_h = S(260.f);

  ImGui::PushFont(fw::theme::g_fonts.large);
  ImGui::TextColored(kPalette.accent, "SETUP");
  ImGui::PopFont();
  ImGui::TextDisabled(
      "Connections live here so the rest of the app stays clean.");
  ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));

  ImGui::BeginChild("card_bus", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("RS485 BUS", "bus_pill",
                        bus.IsConnecting() ? "connecting"
                        : bus.IsOpen()
                            ? (bus.BusFault() ? "fault" : "connected")
                            : "offline",
                        bus.IsOpen(), bus.BusFault());
  ImGui::Spacing();
  if (ImGui::SmallButton("Refresh##bus")) {
    RefreshPortLists();
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##rs485",
                        serial_path_buf[0] ? serial_path_buf : "(none)")) {
    for (std::size_t i = 0; i < serial_ports.size(); ++i) {
      const bool sel =
          (std::strcmp(serial_path_buf, serial_ports[i].c_str()) == 0);
      if (ImGui::Selectable(serial_ports[i].c_str(), sel)) {
        std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                      serial_ports[i].c_str());
        serial_port_index = static_cast<int>(i);
        MarkSettingsDirty();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::InputText("##path", serial_path_buf, sizeof(serial_path_buf));
  ImGui::Checkbox("Auto-reconnect on launch", &auto_reconnect);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    MarkSettingsDirty();
  }
  if (!bus.IsOpen() && !bus.IsConnecting()) {
    if (fw::ui::GlowButton("Connect bus", ImVec2(-1, 0))) {
      RequestConnectBus();
    }
    if (serial_path_buf[0] &&
        fw::ui::GlowButton("Reconnect last", ImVec2(-1, 0))) {
      RequestConnectBus();
    }
  } else if (bus.IsConnecting()) {
    ImGui::BeginDisabled(true);
    fw::ui::GlowButton("Connecting…", ImVec2(-1, 0));
    ImGui::EndDisabled();
  } else {
    if (fw::ui::GlowButton("Disconnect", ImVec2(-1, 0), true)) {
      DisconnectBus();
    }
  }
  ImGui::TextDisabled("timeouts %u  errs %u", bus.TimeoutCount(),
                      bus.ErrCount());
  ImGui::EndChild();

  ImGui::SameLine(0.f, gap);
  ImGui::BeginChild("card_midi", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  std::string midi_preview = "(auto Launchkey)";
  if (midi_port_index >= 0 &&
      midi_port_index < static_cast<int>(midi_ports.size())) {
    midi_preview =
        std::to_string(
            midi_ports[static_cast<std::size_t>(midi_port_index)].index) +
        ": " + midi_ports[static_cast<std::size_t>(midi_port_index)].name;
  }
  fw::ui::SectionHeader("MIDI", "midi_pill",
                        midi_open ? midi.PortName().c_str() : "closed",
                        midi_open);
  ImGui::Spacing();
  if (ImGui::SmallButton("Refresh##midi")) {
    RefreshPortLists();
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##midi", midi_preview.c_str())) {
    if (ImGui::Selectable("(auto Launchkey)", midi_port_index < 0)) {
      midi_port_index = -1;
      MarkSettingsDirty();
    }
    for (const auto &p : midi_ports) {
      const bool sel = (midi_port_index == static_cast<int>(p.index));
      const std::string label = std::to_string(p.index) + ": " + p.name;
      if (ImGui::Selectable(label.c_str(), sel)) {
        midi_port_index = static_cast<int>(p.index);
        MarkSettingsDirty();
      }
    }
    ImGui::EndCombo();
  }
  if (!midi_open) {
    if (fw::ui::GlowButton("Open MIDI", ImVec2(-1, 0))) {
      ConnectMidi();
    }
  } else {
    if (fw::ui::GlowButton("Close MIDI", ImVec2(-1, 0), true)) {
      DisconnectMidi();
    }
  }
  ImGui::EndChild();

  ImGui::SameLine(0.f, gap);
  ImGui::BeginChild("card_out", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("OUTPUT");
  ImGui::Spacing();
  {
    const char *out_items[] = {"Speakers", "Card", "Both"};
    int om = static_cast<int>(out_mode);
    fw::ui::SegmentedControl("##outmode", out_items, 3, &om);
    if (static_cast<OutMode>(om) != out_mode) {
      out_mode = static_cast<OutMode>(om);
      if (out_mode == OutMode::Card) {
        ShutdownAudio();
      }
      MarkSettingsDirty();
    }
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##gain", &gain_db, 0, 127, "Gain %d dB")) {
    if (bus.IsOpen()) {
      const auto r = bus.QueueGain(static_cast<uint8_t>(gain_db));
      NotifyEnqueue(r == BusQueueResult::Ok, QueueResultMsg(r));
    }
    MarkSettingsDirty();
  }
  const float half_w =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (fw::ui::GlowButton("Silence", ImVec2(half_w, 0), true)) {
    AllNotesOff();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Recover", ImVec2(half_w, 0))) {
    bus.RequestRecover(log);
  }
  ImGui::EndChild();

  ImGui::Dummy(ImVec2(0, Metrics::SpaceM));
  ImGui::BeginChild("cdc_hint", ImVec2(0, 96.f), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("WAVE USB CDC");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Wave uploads use a separate Channel Card USB port (cu.usbmodem…). "
      "Configure it on the Waves page — keep RS485 on the adapter above.");
  if (fw::ui::GlowButton("Go to Waves", ImVec2(180.f, 0))) {
    view = GuiView::Waves;
    MarkSettingsDirty();
  }
  ImGui::EndChild();
}

void App::DrawLab()
{
  fw::ui::BeginSection("lab_main", "LAB", ImVec2(0, 0));
  ImGui::Spacing();
  if (!bus.IsOpen()) {
    ImGui::TextColored(kPalette.warning,
                       "Connect RS485 in Setup to send raw commands.");
  }
  ImGui::TextDisabled("Raw console (single-flight via bus worker)");
  const char *targets[] = {"channel", "effect", "all"};
  ImGui::Combo("Target", &raw_target, targets, 3);

  const bool enter =
      ImGui::InputText("Command", raw_cmd, sizeof(raw_cmd),
                       ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemActive()) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !lab_history.empty()) {
      if (lab_history_index < 0) {
        lab_history_index = static_cast<int>(lab_history.size()) - 1;
      } else if (lab_history_index > 0) {
        --lab_history_index;
      }
      std::snprintf(raw_cmd, sizeof(raw_cmd), "%s",
                    lab_history[static_cast<std::size_t>(lab_history_index)]
                        .c_str());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && lab_history_index >= 0) {
      if (lab_history_index + 1 < static_cast<int>(lab_history.size())) {
        ++lab_history_index;
        std::snprintf(raw_cmd, sizeof(raw_cmd), "%s",
                      lab_history[static_cast<std::size_t>(lab_history_index)]
                          .c_str());
      } else {
        lab_history_index = -1;
        raw_cmd[0] = '\0';
      }
    }
  }
  ImGui::SameLine();
  const bool send = fw::ui::GlowButton("Send") || enter;
  if (send && raw_cmd[0]) {
    const protocol::Target t = (raw_target == 1)   ? protocol::Target::Effect
                            : (raw_target == 2) ? protocol::Target::All
                                                : protocol::Target::Channel;
    const auto r = bus.QueueExec(t, raw_cmd);
    NotifyEnqueue(r == BusQueueResult::Ok, QueueResultMsg(r));
    if (lab_history.empty() || lab_history.back() != raw_cmd) {
      lab_history.push_back(raw_cmd);
      while (lab_history.size() > 40) {
        lab_history.pop_front();
      }
    }
    lab_history_index = -1;
  }
  ImGui::TextDisabled("Hints from card help: h · cpu · n0 · g · en · f · w");
  if (ImGui::Button("Channel help  h")) {
    NotifyEnqueue(bus.QueueExec(protocol::Target::Channel, "h") ==
                      BusQueueResult::Ok,
                  "bus offline");
  }
  ImGui::SameLine();
  if (ImGui::Button("cpu 0")) {
    NotifyEnqueue(bus.QueueExec(protocol::Target::Channel, "cpu 0") ==
                      BusQueueResult::Ok,
                  "bus offline");
  }
  ImGui::SameLine();
  if (ImGui::Button("cpu 4")) {
    NotifyEnqueue(bus.QueueExec(protocol::Target::Channel, "cpu 4") ==
                      BusQueueResult::Ok,
                  "bus offline");
  }
  ImGui::SameLine();
  if (ImGui::Button("Effect s")) {
    NotifyEnqueue(bus.QueueExec(protocol::Target::Effect, "s") ==
                      BusQueueResult::Ok,
                  "bus offline");
  }
  fw::ui::EndSection();
}

void App::Draw()
{
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("CMI", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGuiIO &io = ImGui::GetIO();
  if (!io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
      view = GuiView::Perform;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2)) {
      view = GuiView::Tone;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3)) {
      view = GuiView::Waves;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_4)) {
      view = GuiView::Effect;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_5)) {
      view = GuiView::Lab;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_6)) {
      view = GuiView::Setup;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Slash) && io.KeyShift) {
      show_shortcuts = !show_shortcuts;
    }
  }

  const char *nav_labels[] = {"Perform", "Tone", "Waves",
                              "Effect",  "Lab",  "Setup"};
  int vi = static_cast<int>(view);
  const bool nav_was = nav_expanded;
  fw::ui::NavDrawer("##nav", nav_labels, 6, &vi, &nav_expanded, &nav_width);
  if (vi != static_cast<int>(view) || nav_was != nav_expanded) {
    MarkSettingsDirty();
  }
  view = static_cast<GuiView>(vi);

  ImGui::SameLine(0.f, S(Metrics::SpaceM));
  ImGui::BeginChild("main_col", ImVec2(0, 0), ImGuiChildFlags_None);

  // ── Top bar: status left, output cluster right ─────────────────────────
  ImGui::BeginChild("topbar", ImVec2(0, S(Metrics::TopBarH)),
                    ImGuiChildFlags_None);
  fw::ui::StatusPill("tb_bus",
                     bus.IsConnecting() ? "BUS …"
                     : bus.IsOpen()
                         ? (bus.BusFault() ? "BUS FAULT" : "BUS")
                         : "BUS OFF",
                     bus.IsOpen() || bus.IsConnecting(), bus.BusFault());
  ImGui::SameLine();
  fw::ui::StatusPill("tb_midi", midi_open ? "MIDI" : "MIDI OFF", midi_open);
  ImGui::SameLine();
  fw::ui::StatusPill("tb_mode", play_mode == 1 ? "WAVE" : "NOTES", true);
  if (waves.Busy()) {
    ImGui::SameLine();
    fw::ui::StatusPill("tb_up", "UPLOADING", true);
  }

  const float kGainW = S(160.f);
  const float kBtnW = S(90.f);
  const float kGap = S(Metrics::SpaceS);
  const float right_cluster = kGainW + S(36.f) + kGap + kBtnW + kGap + kBtnW;
  const float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() +
                  std::max(0.f, right - right_cluster - S(12.f)));
  // Subtle separator between status and actions.
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x - S(10.f), p.y + S(6.f)),
                ImVec2(p.x - S(10.f), p.y + S(Metrics::TopBarH) - S(10.f)),
                fw::theme::U32A(kPalette.border, 0.7f), 1.f);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Gain");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(kGainW);
  if (ImGui::SliderInt("##top_gain", &gain_db, 0, 127, "%d dB")) {
    if (bus.IsOpen()) {
      const auto r = bus.QueueGain(static_cast<uint8_t>(gain_db));
      NotifyEnqueue(r == BusQueueResult::Ok, QueueResultMsg(r));
    }
    MarkSettingsDirty();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("CS4304 atten %d dB (0 = loudest)", gain_db);
  }
  ImGui::SameLine(0.f, kGap);
  if (fw::ui::GlowButton("Silence", ImVec2(kBtnW, S(28.f)), true)) {
    AllNotesOff();
  }
  ImGui::SameLine(0.f, kGap);
  if (fw::ui::GlowButton("Recover", ImVec2(kBtnW, S(28.f)))) {
    bus.RequestRecover(log);
  }
  ImGui::EndChild();

  DrawBusFaultBanner();

  const float kSplit = S(Metrics::Splitter);
  const float kLogMin = S(56.f);
  const float kLogCollapsed = S(Metrics::StatusBarH + 4.f);
  const float kBodyMin = S(160.f);
  const float kStatus = S(Metrics::StatusBarH);
  const float main_avail = ImGui::GetContentRegionAvail().y - kStatus;
  float log_h = log_collapsed ? kLogCollapsed : layout_log_h;
  if (!log_collapsed) {
    layout_log_h =
        std::clamp(layout_log_h, kLogMin,
                   std::max(kLogMin, main_avail - kBodyMin - kSplit));
    log_h = layout_log_h;
  }
  const float body_h = main_avail - log_h - (log_collapsed ? 0.f : kSplit);
  ImGui::BeginChild("body", ImVec2(0, std::max(body_h, kBodyMin)),
                    ImGuiChildFlags_None);

  if (view == GuiView::Perform) {
    const auto &slots = bank.Slots();
    const int base = 60 + piano_octave * 12;
    static const int kOffsets[] = {0, 2, 4, 5, 7, 9, 11, 12};
    static const char *kNames[] = {"C", "D", "E", "F", "G", "A", "B", "C"};

    constexpr float kScopeMin = 120.f;
    constexpr float kVoiceMin = 56.f;
    constexpr float kPianoMin = 72.f;
    const float perform_avail = ImGui::GetContentRegionAvail().y;
    const float fixed_splits = kSplit * 2.f;
    layout_perform_voice_h =
        std::clamp(layout_perform_voice_h, kVoiceMin, 160.f);
    layout_perform_piano_h =
        std::clamp(layout_perform_piano_h, kPianoMin, 180.f);
    float voice_h = layout_perform_voice_h;
    float piano_h = layout_perform_piano_h;
    float scope_h = perform_avail - voice_h - piano_h - fixed_splits;
    if (scope_h < kScopeMin) {
      const float deficit = kScopeMin - scope_h;
      const float shrink_voice =
          std::min(deficit, std::max(0.f, voice_h - kVoiceMin));
      voice_h -= shrink_voice;
      piano_h -= (deficit - shrink_voice);
      piano_h = std::max(piano_h, kPianoMin);
      scope_h = perform_avail - voice_h - piano_h - fixed_splits;
      layout_perform_voice_h = voice_h;
      layout_perform_piano_h = piano_h;
    }

    fw::ui::BeginSection("scope_hero", "PREVIEW SCOPE", ImVec2(0, scope_h));
    ImGui::SameLine();
    ImGui::TextDisabled("Local sine mix — not Channel DSP / DAC");
    ImGui::Spacing();
    {
      const ImGuiStyle &st = ImGui::GetStyle();
      auto Meas = [&](const char *label, const char *value) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);
        ImGui::PushFont(fw::theme::g_fonts.hero);
        ImGui::TextUnformatted(value);
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(0.f, st.ItemSpacing.x * 2.f);
      };
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.3f",
                    static_cast<double>(preview.Peak()));
      Meas("PEAK", buf);
      std::snprintf(buf, sizeof(buf), "%.3f",
                    static_cast<double>(preview.Rms()));
      Meas("RMS", buf);
      std::snprintf(buf, sizeof(buf), "%.3f",
                    static_cast<double>(preview.PkPk()));
      Meas("Pk-Pk", buf);
      std::snprintf(buf, sizeof(buf), "%d / 16", preview.ActiveCount());
      Meas("VOICES", buf);
      std::snprintf(buf, sizeof(buf), "%.1f ms",
                    static_cast<double>(preview.WindowMs()));
      Meas("WINDOW", buf);
      Meas("RATE", "48 kHz");
      ImGui::NewLine();
    }
    const float meter_h = S(12.f);
    const float header_used = ImGui::GetCursorPosY();
    const float wave_h =
        std::max(scope_h - header_used - meter_h -
                     ImGui::GetStyle().ItemSpacing.y * 2.f - S(8.f),
                 S(80.f));
    fw::ui::GlowWaveform("scope_wave", preview.Samples().data(),
                         PreviewScope::kDisplaySamples, ImVec2(-1, wave_h));
    fw::ui::LevelMeter("peak_meter", preview.Peak(), ImVec2(-1, meter_h),
                       peak_hold);
    fw::ui::EndSection();

    layout_perform_voice_h = std::clamp(
        layout_perform_voice_h - fw::ui::Splitter("##scope_voice", true),
        kVoiceMin, 160.f);

    fw::ui::BeginSection("voice_strip", "VOICES", ImVec2(0, voice_h));
    ImGui::SameLine();
    ImGui::TextDisabled("%u active", bank.ActiveCount());
    ImGui::Spacing();
    {
      const float detail_w = S(220.f);
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      ImGui::BeginChild("voices",
                        ImVec2(ImGui::GetContentRegionAvail().x - detail_w -
                                   gap,
                               0),
                        ImGuiChildFlags_None);
      const float dt = ImGui::GetIO().DeltaTime;
      for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
        const auto &s = slots[i];
        voice_glow[i] =
            fw::anim::ExpApproach(voice_glow[i], s.active ? 1.f : 0.f, dt,
                                  s.active ? 18.f : 4.f);
        ImGui::PushID(static_cast<int>(i));
        char label[4];
        std::snprintf(label, sizeof(label), "n%c", "0123456789abcdef"[i]);
        char sub[16] = {};
        if (s.active) {
          std::snprintf(sub, sizeof(sub), "%s",
                        midi_host::MidiNoteName(s.midi_key).c_str());
        }
        const bool sel = (selected_voice == static_cast<int>(i));
        fw::ui::VoiceSlotBadge("badge", label, s.active, voice_glow[i],
                               s.active ? sub : nullptr, sel);
        if (ImGui::IsItemClicked()) {
          selected_voice = static_cast<int>(i);
        }
        if (i + 1 < midi_host::kVoiceCount) {
          ImGui::SameLine(0.f, S(3.f));
        }
        ImGui::PopID();
      }
      ImGui::EndChild();

      ImGui::SameLine(0.f, gap);
      ImGui::BeginChild("voice_detail", ImVec2(0, 0), ImGuiChildFlags_None);
      const int sv = std::clamp(selected_voice, 0, 15);
      const auto &s = slots[static_cast<std::size_t>(sv)];
      ImGui::TextDisabled("SELECTED");
      ImGui::SameLine();
      ImGui::TextColored(kPalette.accent, "n%x", sv);
      if (s.active) {
        ImGui::Text("%s  ·  %.2f Hz",
                    midi_host::MidiNoteName(s.midi_key).c_str(),
                    static_cast<double>(s.freq_hz));
      } else {
        ImGui::TextDisabled("silent");
      }
      const float btn_w =
          (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
          0.5f;
      if (fw::ui::GlowButton("Envelope", ImVec2(btn_w, S(26.f)))) {
        view = GuiView::Tone;
        MarkSettingsDirty();
      }
      ImGui::SameLine();
      if (fw::ui::GlowButton("Waves", ImVec2(btn_w, S(26.f)))) {
        view = GuiView::Waves;
        MarkSettingsDirty();
      }
      ImGui::EndChild();
    }
    fw::ui::EndSection();

    layout_perform_piano_h = std::clamp(
        layout_perform_piano_h - fw::ui::Splitter("##voice_piano", true),
        kPianoMin, 180.f);

    fw::ui::BeginSection("piano", "KEYBOARD", ImVec2(0, piano_h));
    ImGui::SameLine();
    ImGui::TextDisabled("A–K / W E T Y U · Z/X octave (%d)", piano_octave);
    ImGui::Spacing();
    const float key_gap = ImGui::GetStyle().ItemSpacing.x;
    const float key_w =
        (ImGui::GetContentRegionAvail().x - key_gap * 7.f) / 8.f;
    const float key_h = ImGui::GetContentRegionAvail().y;
    for (int i = 0; i < 8; ++i) {
      if (i) {
        ImGui::SameLine();
      }
      ImGui::PushID(i);
      const int midi_key = base + kOffsets[i];
      bool sounding = false;
      for (const auto &slot : slots) {
        if (slot.active && slot.midi_key == midi_key) {
          sounding = true;
          break;
        }
      }
      char name[8];
      std::snprintf(name, sizeof(name), "%s%d", kNames[i],
                    4 + piano_octave + (i == 7 ? 1 : 0));
      fw::ui::PianoKey("key", name, ImVec2(key_w, key_h), sounding);
      if (ImGui::IsItemActivated() && midi_key >= 0 && midi_key <= 127) {
        ApplyBankEvents(bank.NoteOn(static_cast<uint8_t>(midi_key)));
      }
      if (ImGui::IsItemDeactivated() && midi_key >= 0 && midi_key <= 127) {
        ApplyBankEvents(bank.NoteOff(static_cast<uint8_t>(midi_key)));
      }
      ImGui::PopID();
    }
    fw::ui::EndSection();
  } else if (view == GuiView::Tone) {
    if (!bus.IsOpen()) {
      DrawDisconnectedHint("Connect RS485 in Setup to send tone / envelope.");
    } else {
      DrawPlayModeStrip(*this);
      ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));
      const float tone_avail_w = ImGui::GetContentRegionAvail().x;
      constexpr float kToneLeftMin = 220.f;
      constexpr float kToneRightMin = 280.f;
      if (layout_tone_left_w <= 0.f) {
        layout_tone_left_w = tone_avail_w * 0.34f;
      }
      layout_tone_left_w = std::clamp(
          layout_tone_left_w, kToneLeftMin,
          std::max(kToneLeftMin, tone_avail_w - kToneRightMin - kSplit));

      ImGui::BeginChild("tone_left", ImVec2(layout_tone_left_w, 0),
                        ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
      if (play_mode == 0) {
        DrawOscillatorCard(*this);
        ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));
      } else {
        fw::ui::BeginSection("osc_hidden", "OSCILLATOR", ImVec2(0, S(72.f)));
        ImGui::Spacing();
        ImGui::TextDisabled("Oscillator shapes are notes-mode only.");
        ImGui::TextWrapped(
            "Switch to Notes above, or open Waves for sample banks.");
        fw::ui::EndSection();
        ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));
      }
      DrawFilterCard(*this);
      ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));
      if (fw::ui::GlowButton("Open Wave Bank", ImVec2(-1, S(32.f)))) {
        view = GuiView::Waves;
        MarkSettingsDirty();
      }
      ImGui::EndChild();
      const float tone_col_h = ImGui::GetItemRectSize().y;

      ImGui::SameLine(0.f, 0.f);
      layout_tone_left_w = std::clamp(
          layout_tone_left_w +
              fw::ui::Splitter("##tone_cols", false, 0.f, tone_col_h),
          kToneLeftMin,
          std::max(kToneLeftMin, tone_avail_w - kToneRightMin - kSplit));
      ImGui::SameLine(0.f, 0.f);

      ImGui::BeginChild("tone_right", ImVec2(0, 0), ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
      fw::ui::BeginSection("voice_overview", "ALL VOICES", ImVec2(0, S(192.f)));
      ImGui::Spacing();
      DrawVoiceOverview(*this);
      fw::ui::EndSection();
      ImGui::Dummy(ImVec2(0, S(Metrics::SpaceS)));
      DrawEnvelopeEditor(*this);
      ImGui::EndChild();
    }
  } else if (view == GuiView::Waves) {
    DrawWaveBankPage(*this);
  } else if (view == GuiView::Effect) {
    DrawEffectPanel(*this);
  } else if (view == GuiView::Lab) {
    DrawLab();
  } else if (view == GuiView::Setup) {
    DrawSetup();
  }

  ImGui::EndChild();

  if (!log_collapsed) {
    layout_log_h = std::clamp(
        layout_log_h - fw::ui::Splitter("##body_log", true), 56.f, 280.f);
  }

  // Collapsible log
  ImGui::BeginChild("log", ImVec2(0, log_h), ImGuiChildFlags_Borders);
  int err_count = 0;
  for (const auto &line : log_view) {
    if (line.find("err") != std::string::npos ||
        line.find("FAULT") != std::string::npos) {
      ++err_count;
    }
  }
  if (ImGui::SmallButton(log_collapsed ? "Log ▸" : "Log ▾")) {
    log_collapsed = !log_collapsed;
    MarkSettingsDirty();
  }
  ImGui::SameLine();
  if (err_count > 0) {
    ImGui::TextColored(kPalette.danger, "%d", err_count);
    ImGui::SameLine();
  }
  if (log_collapsed) {
    if (!log_view.empty()) {
      const std::string &last = log_view.back();
      const bool err = last.find("err") != std::string::npos ||
                       last.find("FAULT") != std::string::npos;
      if (err) {
        ImGui::TextColored(kPalette.danger, "%s", last.c_str());
      } else {
        ImGui::TextDisabled("%s", last.c_str());
      }
    } else {
      ImGui::TextDisabled("(empty)");
    }
  } else {
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &log_auto_scroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(140.f));
    ImGui::InputTextWithHint("##logfilter", "filter…", log_filter,
                             sizeof(log_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      log.Clear();
    }
    ImGui::Separator();
    ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &line : log_view) {
      if (log_filter[0] &&
          line.find(log_filter) == std::string::npos) {
        continue;
      }
      const bool err = line.find("err") != std::string::npos ||
                       line.find("FAULT") != std::string::npos ||
                       line.find("fault") != std::string::npos;
      if (err) {
        ImGui::TextColored(kPalette.danger, "%s", line.c_str());
      } else {
        ImGui::TextUnformatted(line.c_str());
      }
      if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(line.c_str());
        PushToastOk("copied");
      }
    }
    if (log_auto_scroll) {
      ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
  }
  ImGui::EndChild();

  DrawStatusBar();
  ImGui::EndChild();

  if (show_shortcuts) {
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 80.f),
        ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(S(420.f), 0));
    ImGui::Begin("Shortcuts", &show_shortcuts,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("1–6   Switch views");
    ImGui::TextUnformatted("Space All notes off / silence");
    ImGui::TextUnformatted("?     Toggle this overlay");
    ImGui::TextUnformatted("A–K   Piano whites (Perform)");
    ImGui::TextUnformatted("W E T Y U  Piano blacks");
    ImGui::TextUnformatted("Z / X Octave down / up");
    ImGui::End();
  }

  toasts.Draw();
  ImGui::End();
}
