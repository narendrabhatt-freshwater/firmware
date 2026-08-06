#include "app.hpp"

#include "env_editor.hpp"
#include "note_event.hpp"
#include "rs485/serial_port.hpp"
#include "rs485/types.hpp"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

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

void App::Draw()
{
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("Freshwater Control", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // ── Transport ──────────────────────────────────────────────────────────
  ImGui::TextUnformatted("Freshwater Control");
  ImGui::SameLine();
  ImGui::TextDisabled("Dear ImGui + GLFW · Preview ≠ card DSP");
  ImGui::Separator();

  if (ImGui::Button("Refresh ports")) {
    RefreshPortLists();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(280);
  if (ImGui::BeginCombo("RS485", serial_path_buf[0] ? serial_path_buf
                                                    : "(none)")) {
    for (std::size_t i = 0; i < serial_ports.size(); ++i) {
      const bool sel =
          (std::strcmp(serial_path_buf, serial_ports[i].c_str()) == 0);
      if (ImGui::Selectable(serial_ports[i].c_str(), sel)) {
        std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                      serial_ports[i].c_str());
        serial_port_index = static_cast<int>(i);
      }
      if (sel) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(200);
  ImGui::InputText("Path", serial_path_buf, sizeof(serial_path_buf));

  if (!bus.IsOpen()) {
    if (ImGui::Button("Connect bus")) {
      ConnectBus();
    }
  } else {
    if (ImGui::Button("Disconnect bus")) {
      DisconnectBus();
    }
  }
  ImGui::SameLine();
  if (bus.IsOpen()) {
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), "BUS OK");
  } else {
    ImGui::TextDisabled("bus closed");
  }
  if (bus.BusFault()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "FAULT");
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  const char *out_items[] = {"Speakers", "Card", "Both"};
  int om = static_cast<int>(out_mode);
  if (ImGui::Combo("Output", &om, out_items, 3)) {
    out_mode = static_cast<OutMode>(om);
    if (out_mode == OutMode::Card) {
      ShutdownAudio();
    }
  }

  ImGui::SetNextItemWidth(220);
  std::string midi_preview = "(auto Launchkey)";
  if (midi_port_index >= 0 &&
      midi_port_index < static_cast<int>(midi_ports.size())) {
    midi_preview = std::to_string(midi_ports[static_cast<std::size_t>(
                                       midi_port_index)]
                                      .index) +
                   ": " +
                   midi_ports[static_cast<std::size_t>(midi_port_index)].name;
  }
  if (ImGui::BeginCombo("MIDI", midi_preview.c_str())) {
    const bool auto_sel = (midi_port_index < 0);
    if (ImGui::Selectable("(auto Launchkey)", auto_sel)) {
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
  ImGui::SameLine();
  if (!midi_open) {
    if (ImGui::Button("Open MIDI")) {
      ConnectMidi();
    }
  } else {
    if (ImGui::Button("Close MIDI")) {
      DisconnectMidi();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), "%s",
                       midi.PortName().c_str());
  }

  ImGui::SetNextItemWidth(120);
  if (ImGui::SliderInt("Gain dB", &gain_db, 0, 127)) {
    if (bus.IsOpen()) {
      bus.QueueGain(static_cast<uint8_t>(gain_db));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Silence")) {
    AllNotesOff();
  }
  ImGui::SameLine();
  if (ImGui::Button("Soft recover")) {
    bus.RequestRecover(log);
  }
  ImGui::SameLine();
  ImGui::Text("timeouts %u  errs %u", bus.TimeoutCount(), bus.ErrCount());

  ImGui::Separator();

  // ── View tabs ──────────────────────────────────────────────────────────
  if (ImGui::BeginTabBar("views")) {
    if (ImGui::BeginTabItem("Perform")) {
      view = GuiView::Perform;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Tone")) {
      view = GuiView::Tone;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Effect")) {
      view = GuiView::Effect;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Lab")) {
      view = GuiView::Lab;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  const float log_h = 140.f;
  const float body_h = ImGui::GetContentRegionAvail().y - log_h - 8.f;

  ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_Borders);

  if (view == GuiView::Perform) {
    ImGui::BeginChild("voices", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f,
                                       0),
                      ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("16 voices (host VoiceBank)");
    ImGui::Separator();
    const auto &slots = bank.Slots();
    for (uint8_t i = 0; i < midi_host::kVoiceCount; ++i) {
      const auto &s = slots[i];
      ImGui::PushID(static_cast<int>(i));
      char hex = "0123456789abcdef"[i];
      if (s.active) {
        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.5f, 1.f), "n%c  %s  %.3f Hz",
                           hex, midi_host::MidiNoteName(s.midi_key).c_str(),
                           s.freq_hz);
      } else {
        ImGui::TextDisabled("n%c  —", hex);
      }
      if (ImGui::IsItemClicked()) {
        selected_voice = static_cast<int>(i);
      }
      ImGui::PopID();
    }
    ImGui::Text("active %u / 16", bank.ActiveCount());
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("scope", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Preview scope");
    ImGui::TextDisabled("Local sine mix — not Channel DSP / DAC");
    ImGui::PlotLines("##wf", preview.Samples().data(), PreviewScope::kDisplaySamples,
                     0, nullptr, -1.f, 1.f, ImVec2(-1, 180));
    ImGui::ProgressBar(preview.Peak(), ImVec2(-1, 0), "peak");

    ImGui::Separator();
    ImGui::TextUnformatted("On-screen keys (C4–B4)");
    static const uint8_t kKeys[] = {60, 62, 64, 65, 67, 69, 71, 72};
    static const char *kNames[] = {"C4", "D4", "E4", "F4",
                                   "G4", "A4", "B4", "C5"};
    for (int i = 0; i < 8; ++i) {
      if (i) {
        ImGui::SameLine();
      }
      ImGui::Button(kNames[i], ImVec2(48, 40));
      if (ImGui::IsItemActivated()) {
        auto evs = bank.NoteOn(kKeys[i]);
        ApplyBankEvents(evs);
      }
      if (ImGui::IsItemDeactivated()) {
        auto evs = bank.NoteOff(kKeys[i]);
        ApplyBankEvents(evs);
      }
    }
    ImGui::TextDisabled("Hold mouse on key; or use a MIDI keyboard");
    ImGui::EndChild();
  } else if (view == GuiView::Tone) {
    if (!bus.IsOpen()) {
      ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                         "Connect RS485 bus to send tone / envelope commands.");
    }
    ImGui::BeginChild("tone_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    DrawToneShapeAndFilter(*this);
    DrawEnvelopeEditor(*this);
    ImGui::EndChild();
  } else if (view == GuiView::Effect) {
    DrawEffectPanel(*this);
  } else if (view == GuiView::Lab) {
    ImGui::TextUnformatted("Raw console (single-flight via bus worker)");
    const char *targets[] = {"channel", "effect", "all"};
    ImGui::Combo("Target", &raw_target, targets, 3);
    ImGui::InputText("Command", raw_cmd, sizeof(raw_cmd));
    ImGui::SameLine();
    if (ImGui::Button("Send") && raw_cmd[0]) {
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
    if (ImGui::Button("e:i2c")) {
      bus.QueueExec(rs485::Target::Effect, "i2c");
    }
    ImGui::SameLine();
    if (ImGui::Button("e:ai")) {
      bus.QueueExec(rs485::Target::Effect, "ai");
    }
    ImGui::TextDisabled("Space = Silence (when window focused)");
  }

  ImGui::EndChild();

  // ── Log ────────────────────────────────────────────────────────────────
  ImGui::BeginChild("log", ImVec2(0, log_h), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Bus log");
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
                     line.find("***") != std::string::npos;
    if (err) {
      ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%s", line.c_str());
    } else {
      ImGui::TextUnformatted(line.c_str());
    }
  }
  if (log_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.f);
  }
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::End();
}
