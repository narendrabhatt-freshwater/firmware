#include "app.hpp"

#include "anim.hpp"
#include "env_editor.hpp"
#include "note_event.hpp"
#include "rs485/serial_port.hpp"
#include "rs485/types.hpp"
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
} // namespace

void App::RefreshPortLists()
{
  serial_ports = rs485::SerialPort::ListPorts();
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
    return true;
  } catch (const std::exception &ex) {
    log.Push(std::string("err: MIDI ") + ex.what());
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
  const std::string path = serial_path_buf;
  if (path.empty()) {
    log.Push("err: no RS485 path");
    return false;
  }
  return bus.Open(path, baud, static_cast<uint32_t>(gain_db), log);
}

void App::DisconnectBus()
{
  if (bus.IsOpen()) {
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

void App::Tick()
{
  if (midi_open) {
    std::vector<midi_host::NoteEvent> notes;
    midi.Poll(notes);
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

  preview.SetVoices(bank);
  preview.Render(48000.f);
  log.Snapshot(log_view);
}

void App::DrawSetup()
{
  const float gap = 10.f;
  const float card_w = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
  const float card_h = 220.f;

  ImGui::PushFont(fw::theme::g_fonts.large);
  ImGui::TextColored(kPalette.accent, "SETUP");
  ImGui::PopFont();
  ImGui::TextDisabled(
      "Connections live here so the rest of the app stays clean.");
  ImGui::Spacing();

  ImGui::BeginChild("card_bus", ImVec2(card_w, card_h), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("RS485 BUS", "bus_pill",
                        bus.IsOpen() ? (bus.BusFault() ? "fault" : "connected")
                                     : "offline",
                        bus.IsOpen(), bus.BusFault());
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
      }
    }
    ImGui::EndCombo();
  }
  ImGui::InputText("##path", serial_path_buf, sizeof(serial_path_buf));
  if (!bus.IsOpen()) {
    if (fw::ui::GlowButton("Connect bus", ImVec2(-1, 0))) {
      ConnectBus();
    }
  } else {
    if (fw::ui::GlowButton("Disconnect", ImVec2(-1, 0), true)) {
      DisconnectBus();
    }
  }
  ImGui::TextDisabled("timeouts %u  errs %u", bus.TimeoutCount(),
                      bus.ErrCount());
  ImGui::EndChild();

  ImGui::SameLine(0.f, gap);
  ImGui::BeginChild("card_midi", ImVec2(card_w, card_h),
                    ImGuiChildFlags_Borders);
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
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##midi", midi_preview.c_str())) {
    if (ImGui::Selectable("(auto Launchkey)", midi_port_index < 0)) {
      midi_port_index = -1;
    }
    for (const auto &p : midi_ports) {
      const bool sel = (midi_port_index == static_cast<int>(p.index));
      const std::string label = std::to_string(p.index) + ": " + p.name;
      if (ImGui::Selectable(label.c_str(), sel)) {
        midi_port_index = static_cast<int>(p.index);
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
  {
    const char *out_items[] = {"Speakers", "Card", "Both"};
    int om = static_cast<int>(out_mode);
    fw::ui::SegmentedControl("##outmode", out_items, 3, &om);
    if (static_cast<OutMode>(om) != out_mode) {
      out_mode = static_cast<OutMode>(om);
      if (out_mode == OutMode::Card) {
        ShutdownAudio();
      }
    }
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##gain", &gain_db, 0, 127, "Gain %d dB")) {
    if (bus.IsOpen()) {
      bus.QueueGain(static_cast<uint8_t>(gain_db));
    }
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

  ImGui::Spacing();
  ImGui::BeginChild("cdc_hint", ImVec2(0, 80), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("WAVE USB CDC");
  ImGui::TextWrapped(
      "Wave uploads use a separate Channel Card USB port (cu.usbmodem…). "
      "Configure it on the Waves page — keep RS485 on the adapter above.");
  if (fw::ui::GlowButton("Go to Waves", ImVec2(180, 0))) {
    view = GuiView::Waves;
  }
  ImGui::EndChild();
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

  const char *nav_labels[] = {"Perform", "Tone", "Waves",
                              "Effect",  "Lab",  "Setup"};
  int vi = static_cast<int>(view);
  bool gain_changed = false;
  fw::ui::NavDrawer("##nav", nav_labels, 6, &vi, &nav_expanded, &nav_width,
                    &gain_db, &gain_changed);
  view = static_cast<GuiView>(vi);
  if (gain_changed && bus.IsOpen()) {
    bus.QueueGain(static_cast<uint8_t>(gain_db));
  }

  ImGui::SameLine(0.f, 10.f);
  ImGui::BeginChild("main_col", ImVec2(0, 0), ImGuiChildFlags_None);

  ImGui::BeginChild("topbar", ImVec2(0, 42), ImGuiChildFlags_None);
  fw::ui::StatusPill("tb_bus",
                     bus.IsOpen() ? (bus.BusFault() ? "BUS FAULT" : "BUS")
                                  : "BUS OFF",
                     bus.IsOpen(), bus.BusFault());
  ImGui::SameLine();
  fw::ui::StatusPill("tb_midi", midi_open ? "MIDI" : "MIDI OFF", midi_open);
  ImGui::SameLine();
  fw::ui::StatusPill("tb_mode", play_mode == 1 ? "WAVE" : "NOTES", true);
  ImGui::SameLine();
  if (waves.Busy()) {
    fw::ui::StatusPill("tb_up", "UPLOADING", true);
    ImGui::SameLine();
  }
  const float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.f, right - 200.f));
  if (fw::ui::GlowButton("Silence", ImVec2(90, 28), true)) {
    AllNotesOff();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Recover", ImVec2(90, 28))) {
    bus.RequestRecover(log);
  }
  ImGui::EndChild();

  const float log_h = 100.f;
  const float body_h = ImGui::GetContentRegionAvail().y - log_h - 6.f;
  ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None);

  if (view == GuiView::Perform) {
    const auto &slots = bank.Slots();
    static const uint8_t kKeys[] = {60, 62, 64, 65, 67, 69, 71, 72};
    static const char *kNames[] = {"C4", "D4", "E4", "F4",
                                   "G4", "A4", "B4", "C5"};

    const float hero_h = 110.f;
    const float piano_h = 106.f;
    const float mid_h =
        std::max(ImGui::GetContentRegionAvail().y - hero_h - piano_h -
                     ImGui::GetStyle().ItemSpacing.y * 2.f,
                 0.f);

    ImGui::BeginChild("scope_hero", ImVec2(0, hero_h), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("PREVIEW SCOPE");
    ImGui::SameLine();
    ImGui::TextDisabled("Local sine mix — not Channel DSP / DAC");
    fw::ui::GlowWaveform("scope_wave", preview.Samples().data(),
                         PreviewScope::kDisplaySamples, ImVec2(-1, 36));
    ImGui::TextUnformatted("Peak");
    ImGui::SameLine();
    fw::ui::LevelMeter("peak_meter", preview.Peak(), ImVec2(-1, 14));
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("voices",
                      ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, mid_h),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextUnformatted("VOICES");
    ImGui::SameLine();
    ImGui::TextDisabled("(%u / 16 active)", bank.ActiveCount());
    const float dt = ImGui::GetIO().DeltaTime;
    for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
      const auto &s = slots[i];
      voice_glow[i] = fw::anim::ExpApproach(voice_glow[i], s.active ? 1.f : 0.f,
                                            dt, s.active ? 18.f : 4.f);
      ImGui::PushID(static_cast<int>(i));
      char label[4];
      std::snprintf(label, sizeof(label), "n%c", "0123456789abcdef"[i]);
      char sub[16] = {};
      if (s.active) {
        std::snprintf(sub, sizeof(sub), "%s",
                      midi_host::MidiNoteName(s.midi_key).c_str());
      }
      fw::ui::VoiceSlotBadge("badge", label, s.active, voice_glow[i],
                             s.active ? sub : nullptr);
      if (ImGui::IsItemClicked()) {
        selected_voice = static_cast<int>(i);
      }
      if ((i % 8) != 7) {
        ImGui::SameLine();
      }
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("voice_detail", ImVec2(0, mid_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextUnformatted("SELECTED VOICE");
    {
      const int sv = std::clamp(selected_voice, 0, 15);
      const auto &s = slots[static_cast<std::size_t>(sv)];
      ImGui::PushFont(fw::theme::g_fonts.large);
      ImGui::TextColored(kPalette.accent, "n%x", sv);
      ImGui::PopFont();
      if (s.active) {
        ImGui::Text("%s  ·  %.3f Hz",
                    midi_host::MidiNoteName(s.midi_key).c_str(),
                    static_cast<double>(s.freq_hz));
      } else {
        ImGui::TextDisabled("silent");
      }
      if (fw::ui::GlowButton("Edit envelope", ImVec2(-1, 32))) {
        view = GuiView::Tone;
      }
      if (fw::ui::GlowButton("Wave bank", ImVec2(-1, 32))) {
        view = GuiView::Waves;
      }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::BeginChild("piano", ImVec2(0, piano_h), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("KEYBOARD (C4–C5)");
    const float key_gap = ImGui::GetStyle().ItemSpacing.x;
    const float key_w =
        (ImGui::GetContentRegionAvail().x - key_gap * 7.f) / 8.f;
    const float key_h = ImGui::GetContentRegionAvail().y;
    for (int i = 0; i < 8; ++i) {
      if (i) {
        ImGui::SameLine();
      }
      ImGui::PushID(i);
      bool sounding = false;
      for (const auto &s : slots) {
        if (s.active && s.midi_key == kKeys[i]) {
          sounding = true;
          break;
        }
      }
      fw::ui::PianoKey("key", kNames[i], ImVec2(key_w, key_h), sounding);
      if (ImGui::IsItemActivated()) {
        ApplyBankEvents(bank.NoteOn(kKeys[i]));
      }
      if (ImGui::IsItemDeactivated()) {
        ApplyBankEvents(bank.NoteOff(kKeys[i]));
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
  } else if (view == GuiView::Tone) {
    if (!bus.IsOpen()) {
      ImGui::TextColored(kPalette.warning,
                         "Connect RS485 in Setup to send tone / envelope.");
    }
    DrawPlayModeStrip(*this);
    ImGui::Spacing();
    const float col_gap = 10.f;
    const float left_w = ImGui::GetContentRegionAvail().x * 0.34f;
    ImGui::BeginChild("tone_left", ImVec2(left_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    /* Oscillator shapes only apply in notes mode. */
    if (play_mode == 0) {
      DrawOscillatorCard(*this);
      ImGui::Spacing();
    } else {
      ImGui::BeginChild("osc_hidden", ImVec2(0, 52), ImGuiChildFlags_Borders);
      ImGui::TextDisabled("Oscillator shapes are notes-mode only.");
      ImGui::TextWrapped("Switch to Notes above, or open Waves for sample banks.");
      ImGui::EndChild();
      ImGui::Spacing();
    }
    DrawFilterCard(*this);
    ImGui::Spacing();
    if (fw::ui::GlowButton("Open Wave Bank", ImVec2(-1, 32))) {
      view = GuiView::Waves;
    }
    ImGui::EndChild();
    ImGui::SameLine(0.f, col_gap);
    ImGui::BeginChild("tone_right", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::BeginChild("voice_overview", ImVec2(0, 192), ImGuiChildFlags_Borders);
    DrawVoiceOverview(*this);
    ImGui::EndChild();
    ImGui::Spacing();
    DrawEnvelopeEditor(*this);
    ImGui::EndChild();
  } else if (view == GuiView::Waves) {
    DrawWaveBankPage(*this);
  } else if (view == GuiView::Effect) {
    DrawEffectPanel(*this);
  } else if (view == GuiView::Lab) {
    ImGui::TextUnformatted("Raw console (single-flight via bus worker)");
    const char *targets[] = {"channel", "effect", "all"};
    ImGui::Combo("Target", &raw_target, targets, 3);
    ImGui::InputText("Command", raw_cmd, sizeof(raw_cmd));
    ImGui::SameLine();
    if (fw::ui::GlowButton("Send") && raw_cmd[0]) {
      const rs485::Target t = (raw_target == 1)   ? rs485::Target::Effect
                              : (raw_target == 2) ? rs485::Target::All
                                                  : rs485::Target::Channel;
      bus.QueueExec(t, raw_cmd);
    }
    if (ImGui::Button("Channel help  h")) {
      bus.QueueExec(rs485::Target::Channel, "h");
    }
    ImGui::SameLine();
    if (ImGui::Button("cpu 0")) {
      bus.QueueExec(rs485::Target::Channel, "cpu 0");
    }
    ImGui::SameLine();
    if (ImGui::Button("cpu 4")) {
      bus.QueueExec(rs485::Target::Channel, "cpu 4");
    }
  } else if (view == GuiView::Setup) {
    DrawSetup();
  }

  ImGui::EndChild();

  ImGui::BeginChild("log", ImVec2(0, log_h), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("LOG");
  ImGui::SameLine();
  ImGui::Checkbox("auto-scroll", &log_auto_scroll);
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    log.Clear();
  }
  ImGui::Separator();
  ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  for (const auto &line : log_view) {
    const bool err = line.find("err") != std::string::npos ||
                     line.find("FAULT") != std::string::npos ||
                     line.find("fault") != std::string::npos;
    if (err) {
      ImGui::TextColored(kPalette.danger, "%s", line.c_str());
    } else {
      ImGui::TextUnformatted(line.c_str());
    }
  }
  if (log_auto_scroll) {
    ImGui::SetScrollHereY(1.f);
  }
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::EndChild();
  ImGui::End();
}
