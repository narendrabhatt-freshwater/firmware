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
#include <cmath>
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
  ImGui::BeginChild("fault_banner", ImVec2(0, 36.f), ImGuiChildFlags_Borders);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kPalette.danger, "BUS FAULT — note TX halted");
  ImGui::SameLine();
  if (fw::ui::GlowButton("Recover", ImVec2(100.f, 26.f))) {
    bus.RequestRecover(log);
    PushToastOk("recover queued");
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::DrawTopNav()
{
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::BeginChild("topnav", ImVec2(0, Metrics::TopNavH), ImGuiChildFlags_None);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f);
  ImGui::PushFont(fw::theme::g_fonts.large);
  ImGui::TextColored(kPalette.text, "FRESHWATER");
  ImGui::PopFont();
  ImGui::SameLine(0.f, 28.f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
  const char *tabs[] = {"PERFORM", "TONE", "WAVES", "EFFECT", "LAB", "SETUP"};
  int vi = static_cast<int>(view);
  if (fw::ui::TopTabs("##tabs", tabs, 6, &vi)) {
    view = static_cast<GuiView>(vi);
    MarkSettingsDirty();
  }
  // Master gain readout right
  char gain_lbl[32];
  std::snprintf(gain_lbl, sizeof(gain_lbl), "MASTER GAIN  -%d dB", gain_db);
  const float gw = ImGui::CalcTextSize(gain_lbl).x + 16.f;
  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - gw);
  if (fw::theme::g_fonts.mono) {
    ImGui::PushFont(fw::theme::g_fonts.mono);
  }
  ImGui::TextColored(kPalette.success, "%s", gain_lbl);
  if (fw::theme::g_fonts.mono) {
    ImGui::PopFont();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void App::DrawActivityLog()
{
  layout_log_w = std::clamp(layout_log_w, 200.f, 420.f);
  ImGui::BeginChild("activity_log", ImVec2(layout_log_w, 0),
                    ImGuiChildFlags_Borders);
  fw::theme::CapsLabel("ACTIVITY LOG", kPalette.text_dim);
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    log.Clear();
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##logfilter", "filter…", log_filter,
                           sizeof(log_filter));
  ImGui::Separator();
  ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  if (fw::theme::g_fonts.mono) {
    ImGui::PushFont(fw::theme::g_fonts.mono);
  }
  for (const auto &line : log_view) {
    if (log_filter[0] && line.find(log_filter) == std::string::npos) {
      continue;
    }
    const bool err = line.find("err") != std::string::npos ||
                     line.find("FAULT") != std::string::npos ||
                     line.find("fault") != std::string::npos ||
                     line.find("Drop") != std::string::npos;
    const bool ok = line.find("ok:") != std::string::npos ||
                    line.find("Established") != std::string::npos;
    if (err) {
      ImGui::TextColored(kPalette.danger, "%s", line.c_str());
    } else if (ok) {
      ImGui::TextColored(kPalette.success, "%s", line.c_str());
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
  if (fw::theme::g_fonts.mono) {
    ImGui::PopFont();
  }
  ImGui::EndChild();
  ImGui::EndChild();
}

void App::DrawFooter()
{
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::BeginChild("footer", ImVec2(0, Metrics::FooterH), ImGuiChildFlags_None);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f);

  auto Dot = [](const ImVec4 &c) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 5.f, p.y + 7.f), 4.f, fw::theme::U32(c));
    ImGui::Dummy(ImVec2(14.f, 14.f));
  };

  fw::theme::CapsLabel("BUS", kPalette.text_dim);
  ImGui::SameLine();
  Dot(bus.IsOpen() && !bus.BusFault() ? kPalette.success
      : bus.BusFault()                  ? kPalette.danger
                                        : kPalette.text_dim);
  ImGui::SameLine(0.f, 0.f);
  ImGui::TextColored(bus.IsOpen() && !bus.BusFault() ? kPalette.success
                     : bus.BusFault()                  ? kPalette.danger
                                                       : kPalette.text_dim,
                     "%s",
                     bus.IsConnecting() ? "CONNECTING"
                     : bus.IsOpen()     ? (bus.BusFault() ? "FAULT" : "ONLINE")
                                        : "OFFLINE");

  ImGui::SameLine(0.f, 18.f);
  fw::theme::CapsLabel("MIDI", kPalette.text_dim);
  ImGui::SameLine();
  Dot(midi_open ? kPalette.success : kPalette.text_dim);
  ImGui::SameLine(0.f, 0.f);
  ImGui::TextColored(midi_open ? kPalette.success : kPalette.text_dim, "%s",
                     midi_open ? "ON" : "OFF");

  ImGui::SameLine(0.f, 18.f);
  fw::theme::CapsLabel("WAVE", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::TextColored(waves.Busy() ? kPalette.accent : kPalette.text_dim, "%s",
                     waves.Busy() ? "UPLOADING" : "IDLE");

  ImGui::SameLine(0.f, 18.f);
  if (fw::ui::GlowButton(play_mode == 1 ? "MODE  WAVE" : "MODE  NOTES",
                         ImVec2(110.f, 28.f))) {
    play_mode = play_mode == 1 ? 0 : 1;
    if (bus.IsOpen()) {
      bus.QueueMode(play_mode == 1 ? protocol::PlayMode::Wave
                                   : protocol::PlayMode::Notes);
    }
    MarkSettingsDirty();
  }

  ImGui::SameLine(0.f, 18.f);
  fw::theme::CapsLabel("GAIN", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.f);
  if (ImGui::SliderInt("##fgain", &gain_db, 0, 127, "0=LOUD  %d")) {
    if (bus.IsOpen()) {
      NotifyEnqueue(bus.QueueGain(static_cast<uint8_t>(gain_db)) ==
                        BusQueueResult::Ok,
                    QueueResultMsg(BusQueueResult::Closed));
    }
    MarkSettingsDirty();
  }

  const float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() +
                  std::max(0.f, right - 260.f));
  if (fw::ui::GlowButton("RECOVER", ImVec2(90.f, 28.f))) {
    bus.RequestRecover(log);
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("SILENCE [SPACE]", ImVec2(150.f, 28.f), true)) {
    AllNotesOff();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
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

  preview.SetTimeDivMs(scope_time_div_ms);
  preview.SetVoltDiv(scope_volt_div);
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


void App::DrawPerform()
{
  using fw::theme::Metrics;
  // Telemetry strip
  ImGui::BeginChild("telemetry", ImVec2(0, Metrics::TelemetryH), ImGuiChildFlags_Borders);
  auto Cell = [&](const char *label, const char *value, const ImVec4 &vcol) {
    ImGui::BeginGroup();
    fw::theme::CapsLabel(label, kPalette.text_dim);
    if (fw::theme::g_fonts.mono) ImGui::PushFont(fw::theme::g_fonts.mono);
    ImGui::TextColored(vcol, "%s", value);
    if (fw::theme::g_fonts.mono) ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(0.f, 28.f);
  };
  char b0[32], b1[32], b2[32], b3[32];
  std::snprintf(b0, sizeof(b0), "48.0 kHz");
  std::snprintf(b1, sizeof(b1), "%02d / 16", preview.ActiveCount());
  const float peak_db = preview.Peak() > 1e-6f
                            ? 20.f * std::log10(preview.Peak())
                            : -60.f;
  const float rms_db = preview.Rms() > 1e-6f
                           ? 20.f * std::log10(preview.Rms())
                           : -60.f;
  std::snprintf(b2, sizeof(b2), "%+.1f / %+.1f dB",
                static_cast<double>(peak_db), static_cast<double>(rms_db));
  std::snprintf(b3, sizeof(b3), "%.2f", static_cast<double>(preview.PkPk()));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f);
  Cell("SAMPLE RATE", b0, kPalette.text);
  Cell("VOICES ACTIVE", b1, kPalette.success);
  Cell("PEAK / RMS", b2, kPalette.danger);
  Cell("PEAK-TO-PEAK", b3, kPalette.text);
  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 120.f);
  fw::theme::CapsLabel("SHORTCUTS", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::TextDisabled("1-6  SPC");
  ImGui::EndChild();

  const float piano_h = std::clamp(layout_perform_piano_h, 110.f, 200.f);
  const float body_h = ImGui::GetContentRegionAvail().y - piano_h - 8.f;

  ImGui::BeginChild("perform_mid", ImVec2(0, body_h), ImGuiChildFlags_None);
  const float voice_w = 320.f;
  const float scope_w = ImGui::GetContentRegionAvail().x - voice_w - 8.f;

  // LOCAL PREVIEW
  ImGui::BeginChild("scope", ImVec2(scope_w, 0), ImGuiChildFlags_Borders);
  fw::theme::CapsLabel("LOCAL PREVIEW", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::TextDisabled("SYNC: FREE");
  ImGui::SameLine();
  ImGui::TextDisabled("TRIG: AUTO");

  const float knobs_w = 100.f;
  const float meters_w = 28.f;
  const float wave_w =
      ImGui::GetContentRegionAvail().x - knobs_w - meters_w - 16.f;
  const float wave_h = ImGui::GetContentRegionAvail().y - 8.f;

  ImGui::BeginChild("wavebox", ImVec2(wave_w, wave_h), ImGuiChildFlags_None);
  const float sc = preview.DisplayScale();
  fw::ui::GlowWaveform("scope_wave", preview.Samples().data(),
                       PreviewScope::kDisplaySamples, ImVec2(-1, -1), -sc, sc);
  ImGui::EndChild();

  ImGui::SameLine(0.f, 6.f);
  ImGui::BeginChild("meters", ImVec2(meters_w, wave_h), ImGuiChildFlags_None);
  fw::ui::VerticalMeter("vm", preview.Peak(), ImVec2(meters_w, wave_h - 4.f),
                        peak_hold);
  ImGui::EndChild();

  ImGui::SameLine(0.f, 8.f);
  ImGui::BeginChild("knobs", ImVec2(knobs_w, wave_h), ImGuiChildFlags_None);
  if (fw::ui::RotaryKnob("##tdiv", "TIME/DIV", &scope_time_div_ms, 1.f, 100.f,
                         "%.0f ms", 72.f)) {
    MarkSettingsDirty();
  }
  ImGui::Dummy(ImVec2(0, 8.f));
  if (fw::ui::RotaryKnob("##vdiv", "VOLT/DIV", &scope_volt_div, 0.05f, 1.f,
                         "%.2f V", 72.f)) {
    MarkSettingsDirty();
  }
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::SameLine(0.f, 8.f);
  // VOICE ALLOCATION 4x4
  ImGui::BeginChild("voices", ImVec2(0, 0), ImGuiChildFlags_Borders);
  fw::theme::CapsLabel("VOICE ALLOCATION", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::TextDisabled("16 CHANNELS");
  const auto &slots = bank.Slots();
  const float gap = 6.f;
  const float cell_w = (ImGui::GetContentRegionAvail().x - gap * 3.f) / 4.f;
  const float cell_h = (ImGui::GetContentRegionAvail().y - 24.f - gap * 3.f) / 4.f;
  const float dt = ImGui::GetIO().DeltaTime;
  for (int i = 0; i < 16; ++i) {
    if (i % 4 != 0) ImGui::SameLine(0.f, gap);
    ImGui::PushID(i);
    const auto &s = slots[static_cast<std::size_t>(i)];
    voice_glow[static_cast<std::size_t>(i)] = fw::anim::ExpApproach(
        voice_glow[static_cast<std::size_t>(i)], s.active ? 1.f : 0.f, dt,
        s.active ? 18.f : 4.f);
    const bool sel = (selected_voice == i);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          s.active ? ImVec4(0.18f, 0.20f, 0.22f, 1.f)
                                   : kPalette.panel_alt);
    ImGui::BeginChild("vc", ImVec2(cell_w, cell_h), ImGuiChildFlags_Borders);
    if (s.active || sel) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetWindowPos();
      dl->AddRectFilled(p, ImVec2(p.x + cell_w, p.y + 3.f),
                        fw::theme::U32(s.active ? kPalette.success
                                                : kPalette.accent));
    }
    char id[8];
    std::snprintf(id, sizeof(id), "n%x", i);
    if (fw::theme::g_fonts.mono) ImGui::PushFont(fw::theme::g_fonts.mono);
    ImGui::TextColored(sel ? kPalette.accent : kPalette.text_dim, "%s", id);
    if (s.active) {
      ImGui::Text("%s", midi_host::MidiNoteName(s.midi_key).c_str());
      ImGui::TextDisabled("%.1fHz", static_cast<double>(s.freq_hz));
    } else {
      ImGui::TextDisabled("--");
      ImGui::TextDisabled("0.0Hz");
    }
    if (fw::theme::g_fonts.mono) ImGui::PopFont();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
      selected_voice = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::EndChild();

  layout_perform_piano_h = std::clamp(
      layout_perform_piano_h - fw::ui::Splitter("##perf_piano", true), 110.f,
      200.f);

  // Piano strip
  ImGui::BeginChild("piano", ImVec2(0, layout_perform_piano_h),
                    ImGuiChildFlags_Borders);
  fw::theme::CapsLabel("OCTAVE SHIFT", kPalette.text_dim);
  ImGui::SameLine();
  if (ImGui::SmallButton("-##oct")) {
    piano_octave = std::max(piano_octave - 1, -2);
    MarkSettingsDirty();
  }
  ImGui::SameLine();
  ImGui::Text("C%d - C%d", 4 + piano_octave, 5 + piano_octave);
  ImGui::SameLine();
  if (ImGui::SmallButton("+##oct")) {
    piano_octave = std::min(piano_octave + 1, 4);
    MarkSettingsDirty();
  }
  ImGui::SameLine(0.f, 24.f);
  fw::theme::CapsLabel("VELOCITY", kPalette.text_dim);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.f);
  ImGui::SliderInt("##vel", &piano_velocity, 1, 127);

  const int base = 60 + piano_octave * 12;
  static const int kOffsets[] = {0, 2, 4, 5, 7, 9, 11, 12};
  static const char *kNames[] = {"C", "D", "E", "F", "G", "A", "B", "C"};
  static const char *kMap[] = {"A", "S", "D", "F", "G", "H", "J", "K"};
  const float key_gap = 4.f;
  const float key_w =
      (ImGui::GetContentRegionAvail().x - key_gap * 7.f) / 8.f;
  const float key_h = ImGui::GetContentRegionAvail().y - 4.f;
  for (int i = 0; i < 8; ++i) {
    if (i) ImGui::SameLine(0.f, key_gap);
    ImGui::PushID(i);
    const int midi_key = base + kOffsets[i];
    bool sounding = false;
    for (const auto &slot : slots) {
      if (slot.active && slot.midi_key == midi_key) {
        sounding = true;
        break;
      }
    }
    char name[16];
    std::snprintf(name, sizeof(name), "%s\n%s", kNames[i], kMap[i]);
    fw::ui::PianoKey("key", name, ImVec2(key_w, key_h), sounding);
    if (ImGui::IsItemActivated() && midi_key >= 0 && midi_key <= 127) {
      ApplyBankEvents(bank.NoteOn(static_cast<uint8_t>(midi_key)));
    }
    if (ImGui::IsItemDeactivated() && midi_key >= 0 && midi_key <= 127) {
      ApplyBankEvents(bank.NoteOff(static_cast<uint8_t>(midi_key)));
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void App::Draw()
{
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPalette.bg);
  ImGui::Begin("CMI", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGuiIO &io = ImGui::GetIO();
  if (!io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_1)) { view = GuiView::Perform; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_2)) { view = GuiView::Tone; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_3)) { view = GuiView::Waves; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_4)) { view = GuiView::Effect; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_5)) { view = GuiView::Lab; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_6)) { view = GuiView::Setup; MarkSettingsDirty(); }
    if (ImGui::IsKeyPressed(ImGuiKey_Slash) && io.KeyShift) {
      show_shortcuts = !show_shortcuts;
    }
  }

  DrawTopNav();
  DrawBusFaultBanner();

  const float footer_h = fw::theme::Metrics::FooterH;
  const float mid_h = ImGui::GetContentRegionAvail().y - footer_h;
  ImGui::BeginChild("mid", ImVec2(0, mid_h), ImGuiChildFlags_None);

  const float log_w = std::clamp(layout_log_w, 200.f, 420.f);
  ImGui::BeginChild("content", ImVec2(-log_w - 6.f, 0), ImGuiChildFlags_None);

  if (view == GuiView::Perform) {
    DrawPerform();
  } else if (view == GuiView::Tone) {
    if (!bus.IsOpen()) {
      DrawDisconnectedHint("Connect RS485 in Setup to send tone / envelope.");
    } else {
      DrawPlayModeStrip(*this);
      ImGui::Spacing();
      const float tone_avail_w = ImGui::GetContentRegionAvail().x;
      constexpr float kToneLeftMin = 220.f;
      constexpr float kToneRightMin = 280.f;
      if (layout_tone_left_w <= 0.f) layout_tone_left_w = tone_avail_w * 0.34f;
      layout_tone_left_w = std::clamp(
          layout_tone_left_w, kToneLeftMin,
          std::max(kToneLeftMin, tone_avail_w - kToneRightMin - 5.f));
      ImGui::BeginChild("tone_left", ImVec2(layout_tone_left_w, 0),
                        ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
      if (play_mode == 0) {
        DrawOscillatorCard(*this);
        ImGui::Spacing();
      }
      DrawFilterCard(*this);
      ImGui::EndChild();
      const float tone_col_h = ImGui::GetItemRectSize().y;
      ImGui::SameLine(0.f, 0.f);
      layout_tone_left_w = std::clamp(
          layout_tone_left_w +
              fw::ui::Splitter("##tone_cols", false, 0.f, tone_col_h),
          kToneLeftMin,
          std::max(kToneLeftMin, tone_avail_w - kToneRightMin - 5.f));
      ImGui::SameLine(0.f, 0.f);
      ImGui::BeginChild("tone_right", ImVec2(0, 0), ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar);
      fw::ui::BeginSection("voice_overview", "ALL VOICES", ImVec2(0, 192.f));
      ImGui::Spacing();
      DrawVoiceOverview(*this);
      fw::ui::EndSection();
      ImGui::Spacing();
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

  ImGui::EndChild(); // content

  ImGui::SameLine(0.f, 6.f);
  DrawActivityLog();

  ImGui::EndChild(); // mid

  DrawFooter();

  if (show_shortcuts) {
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 80.f),
        ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(420.f, 0));
    ImGui::Begin("Shortcuts", &show_shortcuts,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("1-6   Switch modules");
    ImGui::TextUnformatted("Space Silence");
    ImGui::TextUnformatted("?     This overlay");
    ImGui::TextUnformatted("A-K / W E T Y U  Piano");
    ImGui::TextUnformatted("Z / X Octave");
    ImGui::End();
  }

  toasts.Draw();
  ImGui::End();
  ImGui::PopStyleColor();
}
