#include "app.hpp"

#include "anim.hpp"
#include "env_editor.hpp"
#include "note_event.hpp"
#include "host_io/serial_port.hpp"
#include "protocol/types.hpp"
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
  fw::ui::NavDrawer("##nav", nav_labels, 6, &vi, &nav_expanded, &nav_width);
  view = static_cast<GuiView>(vi);

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

  // Gain + Silence/Recover clustered on the right.
  constexpr float kGainW = 160.f;
  constexpr float kBtnW = 90.f;
  constexpr float kGap = 8.f;
  const float right_cluster =
      kGainW + 36.f + kGap + kBtnW + kGap + kBtnW; // label + slider + 2 buttons
  const float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() +
                  std::max(0.f, right - right_cluster));
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Gain");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(kGainW);
  if (ImGui::SliderInt("##top_gain", &gain_db, 0, 127, "%d dB")) {
    if (bus.IsOpen()) {
      bus.QueueGain(static_cast<uint8_t>(gain_db));
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("CS4304 atten %d dB (0 = loudest)", gain_db);
  }
  ImGui::SameLine(0.f, kGap);
  if (fw::ui::GlowButton("Silence", ImVec2(kBtnW, 28), true)) {
    AllNotesOff();
  }
  ImGui::SameLine(0.f, kGap);
  if (fw::ui::GlowButton("Recover", ImVec2(kBtnW, 28))) {
    bus.RequestRecover(log);
  }
  ImGui::EndChild();

  constexpr float kSplit = 5.f;
  constexpr float kLogMin = 56.f;
  constexpr float kBodyMin = 160.f;
  const float main_avail = ImGui::GetContentRegionAvail().y;
  layout_log_h = std::clamp(layout_log_h, kLogMin,
                            std::max(kLogMin, main_avail - kBodyMin - kSplit));
  const float body_h = main_avail - layout_log_h - kSplit;
  ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None);

  if (view == GuiView::Perform) {
    const auto &slots = bank.Slots();
    static const uint8_t kKeys[] = {60, 62, 64, 65, 67, 69, 71, 72};
    static const char *kNames[] = {"C4", "D4", "E4", "F4",
                                   "G4", "A4", "B4", "C5"};

    constexpr float kScopeMin = 120.f;
    constexpr float kVoiceMin = 56.f;
    constexpr float kPianoMin = 72.f;
    const float perform_avail = ImGui::GetContentRegionAvail().y;
    const float fixed_splits = kSplit * 2.f;
    layout_perform_voice_h =
        std::clamp(layout_perform_voice_h, kVoiceMin, 220.f);
    layout_perform_piano_h =
        std::clamp(layout_perform_piano_h, kPianoMin, 220.f);
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
    }

    ImGui::BeginChild("scope_hero", ImVec2(0, scope_h), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("PREVIEW SCOPE");
    ImGui::SameLine();
    ImGui::TextDisabled("Local sine mix — not Channel DSP / DAC");

    // Oscilloscope-style readout strip.
    {
      const ImGuiStyle &st = ImGui::GetStyle();
      auto Meas = [&](const char *label, const char *value) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);
        ImGui::PushFont(fw::theme::g_fonts.large);
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

    const float meter_h = 12.f;
    const float header_used = ImGui::GetCursorPosY();
    const float wave_h =
        std::max(scope_h - header_used - meter_h -
                     ImGui::GetStyle().ItemSpacing.y * 2.f - 8.f,
                 80.f);
    fw::ui::GlowWaveform("scope_wave", preview.Samples().data(),
                         PreviewScope::kDisplaySamples, ImVec2(-1, wave_h));
    fw::ui::LevelMeter("peak_meter", preview.Peak(), ImVec2(-1, meter_h));
    ImGui::EndChild();

    // Drag down → shrink voice strip (scope flexes larger).
    layout_perform_voice_h = std::clamp(
        layout_perform_voice_h - fw::ui::Splitter("##scope_voice", true, kSplit),
        kVoiceMin, 220.f);

    ImGui::BeginChild("voice_strip", ImVec2(0, voice_h), ImGuiChildFlags_Borders);
    {
      const float detail_w = 220.f;
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      ImGui::BeginChild("voices",
                        ImVec2(ImGui::GetContentRegionAvail().x - detail_w -
                                   gap,
                               0),
                        ImGuiChildFlags_None);
      ImGui::TextUnformatted("VOICES");
      ImGui::SameLine();
      ImGui::TextDisabled("%u active", bank.ActiveCount());
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
          ImGui::SameLine(0.f, 3.f);
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
      if (fw::ui::GlowButton("Envelope", ImVec2(btn_w, 26))) {
        view = GuiView::Tone;
      }
      ImGui::SameLine();
      if (fw::ui::GlowButton("Waves", ImVec2(btn_w, 26))) {
        view = GuiView::Waves;
      }
      ImGui::EndChild();
    }
    ImGui::EndChild();

    layout_perform_piano_h = std::clamp(
        layout_perform_piano_h - fw::ui::Splitter("##voice_piano", true, kSplit),
        kPianoMin, 220.f);

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
      for (const auto &slot : slots) {
        if (slot.active && slot.midi_key == kKeys[i]) {
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
    const float tone_col_h = ImGui::GetItemRectSize().y;

    ImGui::SameLine(0.f, 0.f);
    layout_tone_left_w = std::clamp(
        layout_tone_left_w +
            fw::ui::Splitter("##tone_cols", false, kSplit, tone_col_h),
        kToneLeftMin,
        std::max(kToneLeftMin, tone_avail_w - kToneRightMin - kSplit));
    ImGui::SameLine(0.f, 0.f);

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
      const protocol::Target t = (raw_target == 1)   ? protocol::Target::Effect
                              : (raw_target == 2) ? protocol::Target::All
                                                  : protocol::Target::Channel;
      bus.QueueExec(t, raw_cmd);
    }
    if (ImGui::Button("Channel help  h")) {
      bus.QueueExec(protocol::Target::Channel, "h");
    }
    ImGui::SameLine();
    if (ImGui::Button("cpu 0")) {
      bus.QueueExec(protocol::Target::Channel, "cpu 0");
    }
    ImGui::SameLine();
    if (ImGui::Button("cpu 4")) {
      bus.QueueExec(protocol::Target::Channel, "cpu 4");
    }
  } else if (view == GuiView::Setup) {
    DrawSetup();
  }

  ImGui::EndChild();

  layout_log_h = std::clamp(
      layout_log_h - fw::ui::Splitter("##body_log", true, kSplit), kLogMin,
      std::max(kLogMin, main_avail - kBodyMin - kSplit));

  ImGui::BeginChild("log", ImVec2(0, layout_log_h), ImGuiChildFlags_Borders);
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
