#include "app.hpp"

#include "anim.hpp"
#include "env_editor.hpp"
#include "cardlink/midi/note_event.hpp"
#include "cardlink/serial_port.hpp"
#include "cardlink/usb/cdc_port.hpp"
#include "cardlink/usb/stream_proto.hpp"
#include "product.hpp"
#include "cardproto/types.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "sample_ui.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace
{
  using fw::theme::kPalette;
  using fw::theme::Metrics;
  using fw::theme::S;
  using fw::theme::S2;
  using fw::ui::BtnKind;

  const char *QueueResultMsg(BusQueueResult r)
  {
    switch (r)
    {
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

  ImVec4 WithA(const ImVec4 &c, float a) { return ImVec4(c.x, c.y, c.z, a); }

  /** Design bus-status color: online phosphor, connecting warn, fault red. */
  ImVec4 BusColor(const BusController &bus)
  {
    if (bus.BusFault())
    {
      return kPalette.danger;
    }
    if (bus.IsConnecting())
    {
      return kPalette.warning;
    }
    if (bus.IsOpen())
    {
      return kPalette.accent;
    }
    return kPalette.muted;
  }

  const char *BusStatusText(const BusController &bus)
  {
    if (bus.BusFault())
    {
      return "FAULT";
    }
    if (bus.IsConnecting())
    {
      return "CONNECTING";
    }
    return bus.IsOpen() ? "ONLINE" : "OFFLINE";
  }

  /** One text segment of a status-bar chip. */
  struct ChipSeg
  {
    const char *text;
    ImVec4 col;
    ImFont *font;
  };

  /** Rounded status chip: optional dot + text segments + optional caret.
   * Returns true when clicked; out_rect receives the chip rect (for anchoring
   * the popover). */
  bool StatusChip(const char *id, bool dot, const ImVec4 &dot_col, bool dot_glow,
                  const ChipSeg *segs, int nsegs, bool open_bg,
                  const ImVec4 &border_col, bool caret, bool blink,
                  ImVec2 *out_min = nullptr, ImVec2 *out_max = nullptr)
  {
    const float kH = S(22.f);
    const float kPadX = S(10.f);
    const float kGap = S(6.f);
    ImFont *caret_font = fw::theme::g_fonts.mono_small;

    float w = kPadX;
    if (dot)
    {
      w += S(6.f) + kGap;
    }
    for (int i = 0; i < nsegs; ++i)
    {
      w += segs[i].font->CalcTextSizeA(segs[i].font->FontSize, FLT_MAX, 0.f,
                                       segs[i].text)
               .x;
      if (i + 1 < nsegs)
      {
        w += kGap;
      }
    }
    if (caret)
    {
      w += kGap +
           caret_font->CalcTextSizeA(caret_font->FontSize, FLT_MAX, 0.f, "\u25BE")
               .x;
    }
    w += kPadX;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + kH);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##chip", ImVec2(w, kH));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const float alpha = blink ? fw::theme::Blink01() : 1.f;
    const ImVec4 bg = open_bg ? kPalette.panel_alt : kPalette.bg_alt;
    ImVec4 border = border_col;
    if (hovered && !open_bg)
    {
      border = kPalette.border_hi;
    }
    dl->AddRectFilled(p0, p1, fw::theme::U32A(bg, alpha), S(2.f));
    dl->AddRect(p0, p1, fw::theme::U32A(border, border.w * alpha), S(2.f));

    float x = p0.x + kPadX;
    if (dot)
    {
      const ImVec2 c(x + S(3.f), p0.y + kH * 0.5f);
      if (dot_glow)
      {
        dl->AddCircleFilled(c, S(6.f), fw::theme::U32A(dot_col, 0.25f * alpha),
                            14);
      }
      dl->AddCircleFilled(c, S(3.f), fw::theme::U32A(dot_col, alpha), 12);
      x += S(6.f) + kGap;
    }
    for (int i = 0; i < nsegs; ++i)
    {
      const ImVec2 ts = segs[i].font->CalcTextSizeA(segs[i].font->FontSize,
                                                    FLT_MAX, 0.f, segs[i].text);
      dl->AddText(segs[i].font, segs[i].font->FontSize,
                  ImVec2(x, p0.y + (kH - ts.y) * 0.5f),
                  fw::theme::U32A(segs[i].col, alpha), segs[i].text);
      x += ts.x + kGap;
    }
    if (caret)
    {
      const ImVec2 ts = caret_font->CalcTextSizeA(caret_font->FontSize, FLT_MAX,
                                                  0.f, "\u25BE");
      dl->AddText(caret_font, caret_font->FontSize,
                  ImVec2(x, p0.y + (kH - ts.y) * 0.5f),
                  fw::theme::U32A(kPalette.muted, alpha), "\u25BE");
    }

    if (out_min)
    {
      *out_min = p0;
    }
    if (out_max)
    {
      *out_max = p1;
    }
    return clicked;
  }

  void FormatTimebase(float ms, char *buf, std::size_t n)
  {
    if (ms < 1.f)
    {
      std::snprintf(buf, n, "%.0f\u00B5s", static_cast<double>(ms * 1000.f));
    }
    else
    {
      std::snprintf(buf, n, "%gms", static_cast<double>(ms));
    }
  }

  /** One vq-permitted PACK may span two FS frames (2432 B). */
  constexpr double kUsbBodySampPerMsWire =
      static_cast<double>(cardlink::usb::kStreamFrameMax) / 4.0;
  constexpr double kUsbBodySampPerMs =
      static_cast<double>(cardlink::usb::PackMaxSamples(5)) / 2.0;

  double BodyIncOf(double freq_hz, double root_hz)
  {
    double inc = 1.0;
    if (root_hz > 0.0 && freq_hz > 0.0)
    {
      inc = freq_hz / root_hz;
    }
    if (inc > 16.0)
    {
      inc = 16.0;
    }
    if (inc < (1.0 / 16.0))
    {
      inc = 1.0 / 16.0;
    }
    return inc;
  }

  /** Sum sounding voices: 48 × (note Hz / root Hz) samples/ms. */
  void SumUsbBodyLoad(const cardlink::midi::VoiceBank &bank,
                      const cardlink::audio::SampleDryMixer &mixer,
                      double &c4_units, double &samp_ms)
  {
    c4_units = 0.0;
    samp_ms = 0.0;
    const double rate_ms =
        static_cast<double>(cardlink::audio::kSampleRateHz) / 1000.0;
    const auto &slots = bank.Slots();
    for (uint8_t i = 0; i < cardlink::midi::kVoiceCount; ++i)
    {
      const auto &s = slots[i];
      if (!s.active || s.freq_hz <= 0.0)
      {
        continue;
      }
      uint16_t wave = mixer.LiveWave(i);
      if (wave >= cardlink::audio::kAttackWaves)
      {
        wave = s.midi_key;
      }
      double root = mixer.BodyRootHz(wave);
      if (root <= 0.0)
      {
        root = cardlink::audio::kDefaultBodyRootHz;
      }
      const double inc = BodyIncOf(s.freq_hz, root);
      c4_units += inc;
      samp_ms += rate_ms * inc;
    }
  }

  /** Small mono text in a given palette color. */
  void Mono(const char *text, const ImVec4 &col, ImFont *font)
  {
    if (font)
    {
      ImGui::PushFont(font);
    }
    ImGui::TextColored(col, "%s", text);
    if (font)
    {
      ImGui::PopFont();
    }
  }

  /** Underlined phosphor "link" text; returns true on click. */
  bool LinkText(const char *label)
  {
    ImFont *font = fw::theme::g_fonts.caps;
    if (font)
    {
      ImGui::PushFont(font);
    }
    ImGui::TextColored(kPalette.accent, "%s", label);
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 1.f),
                                        ImVec2(mx.x, mx.y - 1.f),
                                        fw::theme::U32A(kPalette.accent, 0.7f));
    if (font)
    {
      ImGui::PopFont();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return ImGui::IsItemClicked();
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
  if (ok)
  {
    return;
  }
  PushToastErr(what ? what : "command dropped");
}

void App::RefreshPortLists()
{
  serial_ports = cardlink::SerialPort::ListPorts();
  midi_ports = cardlink::midi::MidiInput::ListPorts();
  if (serial_path_buf[0] == '\0' && !serial_ports.empty())
  {
    std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                  serial_ports.front().c_str());
  }
}

namespace
{

  bool PathLooksOpenable(const std::string &path)
  {
    if (path.empty())
    {
      return false;
    }
    return access(path.c_str(), R_OK | W_OK) == 0;
  }

  bool IsChannelCardCdc(const std::string &path)
  {
    return path.find("usbmodem") != std::string::npos &&
           path.find("CHCARD") != std::string::npos;
  }

  bool IsUsbModemCdc(const std::string &path)
  {
    return path.find("usbmodem") != std::string::npos ||
           path.find("ttyACM") != std::string::npos;
  }

} // namespace

bool App::EnsureAttackCdc(std::string &err)
{
  RefreshPortLists();

  const std::string cur = attack_cdc_path;
  if (PathLooksOpenable(cur) && IsUsbModemCdc(cur) &&
      !cardlink::usb::LooksLikeRs485AdapterPath(cur))
  {
    return true;
  }

  /* Prefer an online Channel Card CDC. */
  std::string pick;
  for (const auto &p : serial_ports)
  {
    if (IsChannelCardCdc(p) && PathLooksOpenable(p))
    {
      pick = p;
      break;
    }
  }
  if (pick.empty())
  {
    for (const auto &p : serial_ports)
    {
      if (IsUsbModemCdc(p) && !cardlink::usb::LooksLikeRs485AdapterPath(p) &&
          PathLooksOpenable(p))
      {
        pick = p;
        break;
      }
    }
  }

  if (pick.empty())
  {
    err = "err: Channel Card CDC not found — plug in the card "
          "(expect /dev/cu.usbmodemCHCARD*)";
    if (!cur.empty())
    {
      err += "; saved path was ";
      err += cur;
    }
    return false;
  }

  if (cur != pick)
  {
    std::snprintf(attack_cdc_path, sizeof(attack_cdc_path), "%s", pick.c_str());
    MarkSettingsDirty();
    log.Push(std::string("ok: attack CDC → ") + pick);
  }
  samples.SetCdcPath(attack_cdc_path);
  return true;
}

bool App::EnsureSampleStream()
{
  if (sample_bulk && sample_bulk->Running())
  {
    return true;
  }
  if (!sample_bulk)
  {
    sample_bulk = std::make_unique<cardlink::audio::SampleBulkOut>();
  }
  sample_bulk->BindMixer(samples.Mixer());
  std::string err;
  if (!sample_bulk->Start(err))
  {
    log.Push("err: " + err);
    PushToastErr(err);
    return false;
  }
  log.Push("ok: BODY stream open");
  return true;
}

bool App::EnsureAudio()
{
  if (audio_open)
  {
    return true;
  }
  try
  {
    audio = std::make_unique<cardlink::audio::AudioEngine>();
    audio->Start(48000);
    audio_open = true;
    log.Push(std::string("ok: speakers ") + audio->DeviceName());
    return true;
  }
  catch (const std::exception &ex)
  {
    log.Push(std::string("err: audio ") + ex.what());
    PushToastErr(std::string("audio: ") + ex.what());
    audio.reset();
    audio_open = false;
    return false;
  }
}

void App::ShutdownAudio()
{
  if (audio)
  {
    audio->Stop();
    audio.reset();
  }
  audio_open = false;
}

bool App::ConnectMidi()
{
  DisconnectMidi();
  try
  {
    std::optional<unsigned> idx;
    if (midi_port_index >= 0)
    {
      idx = static_cast<unsigned>(midi_port_index);
    }
    midi.Open(idx);
    midi_open = true;
    log.Push(std::string("ok: MIDI ") + midi.PortName());
    PushToastOk(std::string("MIDI ") + midi.PortName());
    MarkSettingsDirty();
    return true;
  }
  catch (const std::exception &ex)
  {
    log.Push(std::string("err: MIDI ") + ex.what());
    PushToastErr(std::string("MIDI: ") + ex.what());
    midi_open = false;
    return false;
  }
}

void App::DisconnectMidi()
{
  if (midi_open)
  {
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
  if (path.empty())
  {
    log.Push("err: no RS485 path");
    PushToastErr("no RS485 path");
    return;
  }
  if (bus.IsConnecting())
  {
    PushToastErr("bus still connecting…");
    return;
  }
  bus.RequestOpen(path, baud, static_cast<uint32_t>(gain_db), log);
  MarkSettingsDirty();
}

void App::DisconnectBus()
{
  if (bus.IsOpen() || bus.IsConnecting())
  {
    bus.Close(log);
  }
}

void App::ApplyBankEvents(const std::vector<cardlink::midi::BankEvent> &events)
{
  const bool want_speakers =
      (out_mode == OutMode::Speakers || out_mode == OutMode::Both);
  const bool want_card =
      (out_mode == OutMode::Card || out_mode == OutMode::Both);

  if (want_speakers)
  {
    EnsureAudio();
  }
  if (want_card)
  {
    (void)EnsureSampleStream();
  }

  for (const auto &ev : events)
  {
    if (want_speakers && audio)
    {
      audio->ApplyBankEvent(ev);
    }
    if (want_card && ev.slot < cardlink::audio::kSampleVoices)
    {
      switch (ev.kind)
      {
      case cardlink::midi::BankEventKind::Off:
        samples.NoteOff(ev.slot);
        break;
      case cardlink::midi::BankEventKind::On:
      case cardlink::midi::BankEventKind::Retrig:
        samples.NoteOn(ev.slot, ev.freq_hz, ev.midi_key);
        break;
      case cardlink::midi::BankEventKind::Steal:
        /* The following On reuses this slot. Starting a session for the
         * dropped key races its late SOF against the replacement note. */
        break;
      }
    }
  }
}

void App::ApplyLocalBankEvents(
    const std::vector<cardlink::midi::BankEvent> &events)
{
  const bool want_speakers =
      (out_mode == OutMode::Speakers || out_mode == OutMode::Both);
  if (want_speakers)
  {
    EnsureAudio();
  }
  for (const auto &ev : events)
  {
    if (want_speakers && audio)
    {
      audio->ApplyBankEvent(ev);
    }
  }
}

void App::AllNotesOff()
{
  const auto evs = bank.AllOff();
  ApplyBankEvents(evs);
  samples.AllNotesOff();
  if (bus.IsOpen())
  {
    bus.RequestSilence();
  }
}

void App::HandleKeyboardPiano()
{
  ImGuiIO &io = ImGui::GetIO();
  if (io.WantTextInput || view != GuiView::Perform)
  {
    return;
  }

  // White: A S D F G H J K ; black: W E T Y U ; octave Z/X (design Oct 0-7)
  struct Map
  {
    ImGuiKey key;
    int semitone; // relative to C of current octave
  };
  static const Map kMap[] = {
      {ImGuiKey_A, 0},
      {ImGuiKey_W, 1},
      {ImGuiKey_S, 2},
      {ImGuiKey_E, 3},
      {ImGuiKey_D, 4},
      {ImGuiKey_F, 5},
      {ImGuiKey_T, 6},
      {ImGuiKey_G, 7},
      {ImGuiKey_Y, 8},
      {ImGuiKey_H, 9},
      {ImGuiKey_U, 10},
      {ImGuiKey_J, 11},
      {ImGuiKey_K, 12},
  };
  if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
  {
    piano_octave = std::max(piano_octave - 1, -4);
    MarkSettingsDirty();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_X, false))
  {
    piano_octave = std::min(piano_octave + 1, 3);
    MarkSettingsDirty();
  }
  const int base = 60 + piano_octave * 12; // C4 ± octave
  for (const auto &m : kMap)
  {
    const int note = base + m.semitone;
    if (note < 0 || note > 127)
    {
      continue;
    }
    if (ImGui::IsKeyPressed(m.key, false))
    {
      ApplyBankEvents(bank.NoteOn(static_cast<uint8_t>(note)));
    }
    if (ImGui::IsKeyReleased(m.key))
    {
      ApplyBankEvents(bank.NoteOff(static_cast<uint8_t>(note)));
    }
  }
}

void App::DrawDisconnectedHint(const char *action)
{
  fw::ui::EmptyState("Not connected", action);
  ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - S(160.f)) * 0.5f);
  if (fw::ui::Btn("Open Setup", S2(160.f, 30.f), BtnKind::Primary))
  {
    view = GuiView::Setup;
    MarkSettingsDirty();
  }
}

void App::SendConsole(const char *cmd)
{
  std::string trimmed = cmd ? cmd : "";
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(
                                 trimmed.front())))
  {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back())))
  {
    trimmed.pop_back();
  }
  if (trimmed.empty())
  {
    return;
  }
  if (!bus.IsOpen())
  {
    log.Push(LogKind::Err,
             "err: bus not connected — cannot send \"" + trimmed + "\"");
    return;
  }
  const char tletter = raw_target == 1 ? 'E' : raw_target == 2 ? 'A'
                                                               : 'C';
  log.Push(LogKind::Tx,
           std::string("[") + tletter + "] > " + trimmed);
  const cardproto::Target t = (raw_target == 1)   ? cardproto::Target::Effect
                             : (raw_target == 2) ? cardproto::Target::All
                                                 : cardproto::Target::Channel;
  const auto r = bus.QueueExec(t, trimmed);
  NotifyEnqueue(r == BusQueueResult::Ok, QueueResultMsg(r));
  if (lab_history.empty() || lab_history.back() != trimmed)
  {
    lab_history.push_back(trimmed);
    while (lab_history.size() > 50)
    {
      lab_history.pop_front();
    }
  }
  lab_history_index = -1;
}

void App::Tick()
{
  const float dt = ImGui::GetIO().DeltaTime;
  toasts.Tick(dt);

  /* Last-sent sync from successful Log+Console Exec. */
  {
    const auto patches = bus.DrainUiMirror();
    bool dirty = false;
    for (const auto &p : patches)
    {
      if (p.has_gain_db)
      {
        gain_db = std::clamp(p.gain_db, 0, 127);
        dirty = true;
      }
      if (p.has_shape)
      {
        shape_mode = std::clamp(p.shape_mode, 0, 2);
        shape_param = std::clamp(p.shape_param, 0.1f, 0.9f);
        dirty = true;
      }
      if (p.has_filter_hz)
      {
        filter_hz_f = std::clamp(p.filter_hz_f, 20.f, 20000.f);
        dirty = true;
      }
      if (p.has_filter_q)
      {
        filter_q_f = std::clamp(p.filter_q_f, 0.5f, 10.f);
        dirty = true;
      }
      if (p.has_filter_k)
      {
        filter_k_f = std::clamp(p.filter_k_f, 0.f, 10.f);
        dirty = true;
      }
      if (p.has_filter_bypass)
      {
        filter_bypass = p.filter_bypass;
        dirty = true;
      }
      if (p.has_filter_voice)
      {
        filter_voice = std::clamp(p.filter_voice, 0, 7);
        dirty = true;
      }
      if (p.has_fx_phantom)
      {
        effect.phantom = p.fx_phantom;
        dirty = true;
      }
      if (p.has_fx_audio_en)
      {
        effect.audio_en = p.fx_audio_en;
        dirty = true;
      }
      if (p.has_fx_echo)
      {
        effect.echo = p.fx_echo;
        dirty = true;
      }
      if (p.has_fx_led_flash)
      {
        effect.led_flash = p.fx_led_flash;
        dirty = true;
      }
      if (p.has_fx_led_red)
      {
        effect.led_red = p.fx_led_red;
        dirty = true;
      }
      if (p.has_fx_led_yellow)
      {
        effect.led_yellow = p.fx_led_yellow;
        dirty = true;
      }
      if (p.has_fx_usb_adc_ch)
      {
        effect.usb_adc_ch = std::clamp(p.fx_usb_adc_ch, 1, 8);
        dirty = true;
      }
      if (p.has_note)
      {
        std::vector<cardlink::midi::BankEvent> evs;
        if (p.note_slot < 0)
        {
          evs = bank.SetAllFreq(p.note_hz);
          bus.AcknowledgeAllHz(p.note_hz);
        }
        else
        {
          const uint8_t slot =
              static_cast<uint8_t>(std::clamp(p.note_slot, 0, 7));
          evs = bank.SetSlotFreq(slot, p.note_hz);
          bus.AcknowledgeSlotHz(slot, p.note_hz);
        }
        ApplyLocalBankEvents(evs);
        dirty = true;
      }
    }
    if (dirty)
    {
      MarkSettingsDirty();
    }
  }

  if (settings_dirty)
  {
    settings_save_countdown -= dt;
    if (settings_save_countdown <= 0.f)
    {
      fw::settings::Save(*this);
      settings_dirty = false;
    }
  }

  if (midi_open)
  {
    std::vector<cardlink::midi::NoteEvent> notes;
    midi.Poll(notes);
    if (!notes.empty())
    {
      midi_activity = true;
      midi_activity_timer = 0.18f;
    }
    for (const auto &n : notes)
    {
      std::vector<cardlink::midi::BankEvent> evs;
      if (n.action == cardlink::midi::NoteAction::On)
      {
        evs = bank.NoteOn(n.key);
      }
      else if (n.action == cardlink::midi::NoteAction::AllOff)
      {
        AllNotesOff();
        continue;
      }
      else
      {
        evs = bank.NoteOff(n.key);
      }
      ApplyBankEvents(evs);
    }
  }
  if (midi_activity_timer > 0.f)
  {
    midi_activity_timer -= dt;
    if (midi_activity_timer <= 0.f)
    {
      midi_activity = false;
    }
  }

  HandleKeyboardPiano();

  scope_time_ms = std::clamp(scope_time_ms, 0.1f, 200.f);
  scope_volt = std::clamp(scope_volt, 0.1f, 8.f);
  preview.SetTimeDivUs(scope_time_ms * 1000.f);
  preview.SetVoltDiv(scope_volt);
  preview.SetVoices(bank);
  preview.Render(48000.f);

  const float peak = preview.Peak();
  if (peak >= peak_hold)
  {
    peak_hold = peak;
    peak_hold_timer = 1.2f;
  }
  else
  {
    peak_hold_timer -= dt;
    if (peak_hold_timer <= 0.f)
    {
      peak_hold = std::max(0.f, peak_hold - dt * 0.35f);
    }
  }

  log.Snapshot(log_view);
  poll_log.Snapshot(poll_log_view);

  // Drain SAMPLE file dialogs without blocking the UI thread.
  if (!file_dialog.Busy())
  {
    std::string path;
    if (file_dialog.TakeResult(path) && !path.empty())
    {
      if (sample_file_pick == SampleFilePick::Wave && sample_file_voice >= 0 &&
          sample_file_voice < 8)
      {
        pending_sample_folder = std::string("wave:") +
                                std::to_string(sample_file_voice) + ":" + path;
      }
      sample_file_pick = SampleFilePick::None;
      sample_file_voice = -1;
    }
  }
}

/* ── Left icon sidebar (design: 56 px rail, FW logo, 5 nav items) ────── */

void App::DrawSidebar()
{
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("sidebar", ImVec2(S(Metrics::SidebarW), 0),
                    ImGuiChildFlags_None);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  dl->AddLine(ImVec2(wp.x + ws.x - 1.f, wp.y),
              ImVec2(wp.x + ws.x - 1.f, wp.y + ws.y),
              fw::theme::U32(kPalette.border));

  // Logo block, 40 px, bottom border.
  {
    ImFont *f = fw::theme::g_fonts.mono;
    const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, "FW");
    dl->AddText(f, f->FontSize,
                ImVec2(wp.x + (ws.x - ts.x) * 0.5f,
                       wp.y + (S(40.f) - ts.y) * 0.5f),
                fw::theme::U32(kPalette.accent), "FW");
    dl->AddLine(ImVec2(wp.x, wp.y + S(40.f)),
                ImVec2(wp.x + ws.x, wp.y + S(40.f)),
                fw::theme::U32(kPalette.border));
    ImGui::Dummy(ImVec2(0, S(40.f)));
  }

  struct NavDef
  {
    const char *symbol;
    const char *label;
    const char *key;
    GuiView v;
  };
  static const NavDef kNav[] = {
      {"\u25C8", "PERFORM", "[1]", GuiView::Perform},
      {"\u25C7", "TONE", "[2]", GuiView::Tone},
      {"\u229F", "SAMPLE", "[3]", GuiView::Sample},
      {"\u229E", "EFFECT", "[4]", GuiView::Effect},
      {"\u25E7", "SETUP", "[5]", GuiView::Setup},
  };

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6.f));
  for (const auto &n : kNav)
  {
    const bool active = (view == n.v);
    ImGui::SetCursorPosX(S(4.f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ws.x - S(8.f), S(46.f));
    ImGui::PushID(n.label);
    ImGui::InvisibleButton("##nav", size);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
    {
      view = n.v;
      MarkSettingsDirty();
    }
    ImGui::PopID();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    if (active)
    {
      dl->AddRectFilled(p0, p1, fw::theme::U32A(kPalette.accent, 0.08f),
                        S(2.f));
      dl->AddRect(p0, p1, fw::theme::U32A(kPalette.accent, 0.20f), S(2.f));
    }
    else if (hovered)
    {
      dl->AddRectFilled(p0, p1, fw::theme::U32(kPalette.panel), S(2.f));
    }
    const ImVec4 col = active ? kPalette.accent : kPalette.text_dim;
    ImFont *icf = fw::theme::g_fonts.icons;
    const ImVec2 is = icf->CalcTextSizeA(icf->FontSize, FLT_MAX, 0.f, n.symbol);
    dl->AddText(icf, icf->FontSize,
                ImVec2(p0.x + (size.x - is.x) * 0.5f, p0.y + S(5.f)),
                fw::theme::U32(col), n.symbol);
    ImFont *lf = fw::theme::g_fonts.mono_small;
    const ImVec2 ls = lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0.f, n.label);
    dl->AddText(lf, lf->FontSize,
                ImVec2(p0.x + (size.x - ls.x) * 0.5f, p0.y + S(23.f)),
                fw::theme::U32(col), n.label);
    const ImVec2 ks = lf->CalcTextSizeA(lf->FontSize - 1.f, FLT_MAX, 0.f, n.key);
    dl->AddText(lf, lf->FontSize - 1.f,
                ImVec2(p0.x + (size.x - ks.x) * 0.5f, p0.y + S(34.f)),
                fw::theme::U32(kPalette.muted), n.key);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2.f));
  }

  // Bottom cluster: RCV (on fault) + ? — Silence lives in the status bar.
  const bool fault = bus.IsOpen() && bus.BusFault();
  const float cluster_h = (fault ? S(50.f) : 0.f) + S(28.f);
  ImGui::SetCursorPosY(ws.y - cluster_h - S(6.f));

  auto RailButton = [&](const char *sym, const char *lbl, bool danger_blink,
                        BtnKind kind)
  {
    ImGui::SetCursorPosX(S(6.f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ws.x - S(12.f), S(40.f));
    ImGui::PushID(lbl);
    ImGui::InvisibleButton("##rail", size);
    const bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    const float a = danger_blink ? fw::theme::Blink01() : 1.f;
    const ImVec4 base =
        (kind == BtnKind::Danger) ? kPalette.danger : kPalette.text_dim;
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    if (kind == BtnKind::Danger)
    {
      dl->AddRectFilled(p0, p1, fw::theme::U32A(base, 0.07f * a), S(2.f));
      dl->AddRect(p0, p1, fw::theme::U32A(base, 0.22f * a), S(2.f));
    }
    ImFont *f = fw::theme::g_fonts.mono;
    const ImVec2 ss = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, sym);
    dl->AddText(f, f->FontSize,
                ImVec2(p0.x + (size.x - ss.x) * 0.5f, p0.y + S(5.f)),
                fw::theme::U32A(base, a), sym);
    ImFont *lf = fw::theme::g_fonts.mono_small;
    const ImVec2 ls = lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0.f, lbl);
    dl->AddText(lf, lf->FontSize,
                ImVec2(p0.x + (size.x - ls.x) * 0.5f, p0.y + S(24.f)),
                fw::theme::U32A(base, a), lbl);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.f));
    return clicked;
  };

  if (fault)
  {
    ImGui::Separator();
    if (RailButton("\u26A0", "RCV", true, BtnKind::Danger))
    {
      bus.RequestRecover(log);
      log.Push("Bus fault — recover queued");
    }
  }
  ImGui::Separator();
  {
    ImGui::SetCursorPosX(S(6.f));
    if (fw::ui::ChipBtn("?", false, BtnKind::Neutral))
    {
      show_shortcuts = true;
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

/* ── Top status bar: BUS / MIDI / MODE / VOICES chips + gain + silence ── */

void App::DrawStatusBar()
{
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("statusbar", ImVec2(0, S(Metrics::StatusBarH)),
                    ImGuiChildFlags_None);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 1.f),
              ImVec2(wp.x + ws.x, wp.y + ws.y - 1.f),
              fw::theme::U32(kPalette.border));

  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.caps;

  const bool fault = bus.IsOpen() && bus.BusFault();
  const bool online = bus.IsOpen() && !bus.BusFault();
  const bool connecting = bus.IsConnecting();

  const float chip_y = (S(Metrics::StatusBarH) - S(22.f)) * 0.5f;
  ImGui::SetCursorPos(ImVec2(S(12.f), chip_y));

  // BUS chip
  {
    const ImVec4 sc = fault    ? kPalette.danger
                      : online ? kPalette.accent
                               : kPalette.text;
    ChipSeg segs[3];
    int n = 0;
    segs[n++] = {"BUS", kPalette.text_dim, fs};
    segs[n++] = {BusStatusText(bus), connecting ? kPalette.text : sc, fm};
    std::string port = bus.Path();
    if (online && !port.empty())
    {
      const auto slash = port.rfind('/');
      if (slash != std::string::npos)
      {
        port = port.substr(slash + 1);
      }
      segs[n++] = {port.c_str(), kPalette.text_dim, fs};
    }
    ImVec2 mn, mx;
    const ImVec4 border =
        fault ? WithA(kPalette.danger, 0.4f) : kPalette.border;
    if (StatusChip("bus_chip", true, BusColor(bus), online || fault, segs, n,
                   bus_popover, border, true, fault, &mn, &mx))
    {
      bus_popover = !bus_popover;
      midi_popover = false;
      if (bus_popover)
      {
        RefreshPortLists();
        ImGui::OpenPopup("bus_pop");
      }
    }
    ImGui::SetNextWindowPos(ImVec2(mn.x, mx.y + S(4.f)));
    ImGui::SetNextWindowSize(ImVec2(S(252.f), 0.f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPalette.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, kPalette.border_hi);
    if (ImGui::BeginPopup("bus_pop"))
    {
      fw::ui::StatusDot(3.f, BusColor(bus), online || fault);
      ImGui::SameLine(0.f, S(8.f));
      Mono(BusStatusText(bus), kPalette.text, fm);
      if (connecting)
      {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fw::theme::Blink01());
        Mono("connecting…", kPalette.warning, fs);
        ImGui::PopStyleVar();
      }
      ImGui::Separator();
      if (!online && !connecting && !fault)
      {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##port", serial_path_buf[0]
                                            ? serial_path_buf
                                            : "\u2014 select port \u2014"))
        {
          for (const auto &p : serial_ports)
          {
            if (ImGui::Selectable(p.c_str(),
                                  p == serial_path_buf))
            {
              std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                            p.c_str());
              MarkSettingsDirty();
            }
          }
          ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##manual", "or enter path…",
                                 serial_path_buf, sizeof(serial_path_buf));
        ImGui::BeginDisabled(serial_path_buf[0] == '\0');
        if (fw::ui::Btn("Connect", ImVec2(-1, 0), BtnKind::Primary))
        {
          RequestConnectBus();
          ImGui::CloseCurrentPopup();
          bus_popover = false;
        }
        ImGui::EndDisabled();
      }
      if (online)
      {
        if (fw::ui::Btn("Disconnect", ImVec2(-1, 0), BtnKind::Danger))
        {
          DisconnectBus();
          AllNotesOff();
          ImGui::CloseCurrentPopup();
          bus_popover = false;
        }
      }
      if (fault)
      {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fw::theme::Blink01());
        const bool rec =
            fw::ui::Btn("\u26A0 Recover Fault", ImVec2(-1, 0), BtnKind::Danger);
        ImGui::PopStyleVar();
        if (rec)
        {
          bus.RequestRecover(log);
          ImGui::CloseCurrentPopup();
          bus_popover = false;
        }
      }
      {
        ImFont *lk = fw::theme::g_fonts.mono_small;
        ImGui::PushFont(lk);
        const float tw = ImGui::CalcTextSize("Full setup \u2192").x;
        ImGui::SetCursorPosX((S(252.f) - tw) * 0.5f);
        ImGui::TextColored(kPalette.text_dim, "Full setup \u2192");
        ImGui::PopFont();
        if (ImGui::IsItemClicked())
        {
          view = GuiView::Setup;
          ImGui::CloseCurrentPopup();
          bus_popover = false;
        }
      }
      ImGui::EndPopup();
    }
    else
    {
      bus_popover = false;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.f, S(8.f));
  }

  // MIDI chip
  {
    ChipSeg segs[2];
    int n = 0;
    segs[n++] = {"MIDI", kPalette.text_dim, fs};
    std::string pname = midi_open ? midi.PortName() : "OFF";
    if (pname.size() > 14)
    {
      pname = pname.substr(0, 13) + "…";
    }
    segs[n++] = {midi_open ? pname.c_str() : "OFF",
                 midi_open ? kPalette.accent : kPalette.text_dim, fm};
    ImVec2 mn, mx;
    if (StatusChip("midi_chip", true,
                   midi_open ? kPalette.accent : kPalette.muted, midi_open,
                   segs, n, midi_popover, kPalette.border, true, false, &mn,
                   &mx))
    {
      midi_popover = !midi_popover;
      bus_popover = false;
      if (midi_popover)
      {
        RefreshPortLists();
        ImGui::OpenPopup("midi_pop");
      }
    }
    ImGui::SetNextWindowPos(ImVec2(mn.x, mx.y + S(4.f)));
    ImGui::SetNextWindowSize(ImVec2(S(240.f), 0.f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPalette.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, kPalette.border_hi);
    if (ImGui::BeginPopup("midi_pop"))
    {
      fw::ui::StatusDot(3.f, midi_open ? kPalette.accent : kPalette.muted,
                        midi_open);
      ImGui::SameLine(0.f, S(8.f));
      Mono(midi_open ? "MIDI ON" : "MIDI OFF", kPalette.text, fm);
      if (midi_open)
      {
        ImGui::SameLine();
        Mono(midi.PortName().c_str(), kPalette.text_dim, fs);
      }
      ImGui::Separator();
      std::string midi_preview = "\u2014 auto-pick \u2014";
      if (midi_port_index >= 0 &&
          midi_port_index < static_cast<int>(midi_ports.size()))
      {
        midi_preview =
            midi_ports[static_cast<std::size_t>(midi_port_index)].name;
      }
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##midiport", midi_preview.c_str()))
      {
        if (ImGui::Selectable("\u2014 auto-pick \u2014",
                              midi_port_index < 0))
        {
          midi_port_index = -1;
          MarkSettingsDirty();
        }
        for (const auto &p : midi_ports)
        {
          if (ImGui::Selectable(p.name.c_str(),
                                midi_port_index == static_cast<int>(p.index)))
          {
            midi_port_index = static_cast<int>(p.index);
            MarkSettingsDirty();
          }
        }
        ImGui::EndCombo();
      }
      if (!midi_open)
      {
        if (fw::ui::Btn("Open MIDI", ImVec2(-1, 0), BtnKind::Primary))
        {
          ConnectMidi();
          ImGui::CloseCurrentPopup();
          midi_popover = false;
        }
      }
      else
      {
        if (fw::ui::Btn("Close MIDI", ImVec2(-1, 0), BtnKind::Danger))
        {
          DisconnectMidi();
          ImGui::CloseCurrentPopup();
          midi_popover = false;
        }
      }
      ImGui::EndPopup();
    }
    else
    {
      midi_popover = false;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.f, S(8.f));
  }

  // VOICES chip
  {
    int active = 0;
    for (const auto &s : bank.Slots())
    {
      if (s.active)
      {
        ++active;
      }
    }
    char v[16];
    std::snprintf(v, sizeof(v), "%d/8", active);
    ChipSeg segs[2];
    segs[0] = {"VOICES", kPalette.text_dim, fs};
    segs[1] = {v, active > 0 ? kPalette.accent : kPalette.text_dim, fm};
    StatusChip("voices_chip", true,
               active > 0 ? kPalette.accent : kPalette.muted, active > 0, segs,
               2, false, kPalette.border, false, false);
  }

  // Right cluster: GAIN · [RECOVER] · SILENCE
  {
    const float silence_w = S(96.f);
    const float recover_w = fault ? S(96.f) : 0.f;
    const float gain_w = S(34.f + 48.f + 80.f + 30.f); // labels+slider+value
    const float total = gain_w + S(10.f) + recover_w +
                        (fault ? S(8.f) : 0.f) + silence_w + S(12.f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + S(8.f), ws.x - total));

    ImGui::BeginGroup();
    ImGui::SetCursorPosY(chip_y + S(4.f));
    Mono("GAIN", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(chip_y + S(5.f));
    Mono("0=LOUD", kPalette.warning, fw::theme::g_fonts.mono_small);
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(chip_y + S(2.f));
    ImGui::SetNextItemWidth(S(80.f));
    if (ImGui::SliderInt("##gain", &gain_db, 0, 127, ""))
    {
      if (bus.IsOpen())
      {
        NotifyEnqueue(bus.QueueGain(static_cast<uint8_t>(gain_db)) ==
                          BusQueueResult::Ok,
                      QueueResultMsg(BusQueueResult::Closed));
      }
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(chip_y + S(3.f));
    char g[8];
    std::snprintf(g, sizeof(g), "%d", gain_db);
    Mono(g, kPalette.accent, fw::theme::g_fonts.mono);
    ImGui::EndGroup();

    if (fault)
    {
      ImGui::SameLine(0.f, S(10.f));
      ImGui::SetCursorPosY(chip_y);
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fw::theme::Blink01());
      const bool rec = fw::ui::Btn("\u26A0 RECOVER",
                                   ImVec2(recover_w, S(22.f)),
                                   BtnKind::Danger);
      ImGui::PopStyleVar();
      if (rec)
      {
        bus.RequestRecover(log);
        log.Push("Bus fault — recover queued");
      }
    }
    ImGui::SameLine(0.f, fault ? S(8.f) : S(10.f));
    ImGui::SetCursorPosY(chip_y);
    if (fw::ui::Btn("\u25A0 SILENCE", ImVec2(silence_w, S(22.f)),
                    BtnKind::Danger))
    {
      AllNotesOff();
      log.Push("Silence — all-notes-off");
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Silence all [Space]");
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void App::DrawBusFaultBanner()
{
  if (!bus.IsOpen() || !bus.BusFault())
  {
    return;
  }
  const float a = fw::theme::Blink01();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, WithA(kPalette.danger, 0.10f * a));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14.f), 0.f));
  ImGui::BeginChild("fault_banner", ImVec2(0, S(Metrics::FaultBannerH)),
                    ImGuiChildFlags_None);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 1.f),
              ImVec2(wp.x + ws.x, wp.y + ws.y - 1.f),
              fw::theme::U32A(kPalette.danger, 0.3f * a));
  ImGui::SetCursorPosY(S(8.f));
  Mono("\u26A0 BUS FAULT — Serial link timed out. TX halted.",
       WithA(kPalette.danger, a), fw::theme::g_fonts.mono);
  ImGui::SameLine(ws.x - S(94.f));
  ImGui::SetCursorPosY(S(5.f));
  if (fw::ui::Btn("RECOVER", S2(80.f, 22.f), BtnKind::Danger))
  {
    bus.RequestRecover(log);
    log.Push("Bus fault — recover queued");
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

/* ── Right LOG + CONSOLE panel ───────────────────────────────────────── */

void App::DrawActivityLog()
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;

  const std::deque<LogEntry> &active_view =
      (log_panel_tab == 1) ? poll_log_view : log_view;

  bool has_error = false;
  for (const auto &e : log_view)
  {
    if (e.kind == LogKind::Err)
    {
      has_error = true;
      break;
    }
  }
  bool poll_has_warn = false;
  for (const auto &e : poll_log_view)
  {
    if (e.kind == LogKind::Err)
    {
      poll_has_warn = true;
      break;
    }
  }

  auto KindColor = [&](LogKind k) -> ImVec4
  {
    switch (k)
    {
    case LogKind::Ok:
      return kPalette.accent_dim;
    case LogKind::Err:
      return kPalette.danger;
    case LogKind::Tx:
      return kPalette.warning;
    default:
      return kPalette.text_dim;
    }
  };
  auto KindLabel = [](LogKind k) -> const char *
  {
    switch (k)
    {
    case LogKind::Ok:
      return "OK";
    case LogKind::Err:
      return "ERR";
    case LogKind::Tx:
      return "TX";
    default:
      return "INFO";
    }
  };

  if (log_collapsed)
  {
    // Collapsed 28 px rail — click anywhere to open.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("log_rail", ImVec2(S(Metrics::LogRailW), 0),
                      ImGuiChildFlags_None);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddLine(wp, ImVec2(wp.x, wp.y + ws.y), fw::theme::U32(kPalette.border));
    ImGui::InvisibleButton("##open", ws);
    if (ImGui::IsItemClicked())
    {
      log_collapsed = false;
      MarkSettingsDirty();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("Open log / console");
    }
    float y = wp.y + S(12.f);
    if (has_error || poll_has_warn)
    {
      dl->AddCircleFilled(ImVec2(wp.x + ws.x * 0.5f, y), S(3.f),
                          fw::theme::U32(kPalette.danger), 10);
      y += S(12.f);
    }
    char cnt[16];
    std::snprintf(cnt, sizeof(cnt), "%d", static_cast<int>(log_view.size()));
    const ImVec2 cs = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, cnt);
    dl->AddText(fs, fs->FontSize, ImVec2(wp.x + (ws.x - cs.x) * 0.5f, y),
                fw::theme::U32(kPalette.muted), cnt);
    // Vertical LOG letters
    const char *letters[] = {"L", "O", "G"};
    float ly = wp.y + ws.y * 0.5f - S(18.f);
    for (const char *ch : letters)
    {
      const ImVec2 ts = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, ch);
      dl->AddText(fs, fs->FontSize, ImVec2(wp.x + (ws.x - ts.x) * 0.5f, ly),
                  fw::theme::U32(kPalette.text_dim), ch);
      ly += S(12.f);
    }
    const ImVec2 as = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, "\u25C1");
    dl->AddText(fs, fs->FontSize,
                ImVec2(wp.x + (ws.x - as.x) * 0.5f, wp.y + ws.y - S(20.f)),
                fw::theme::U32(kPalette.muted), "\u25C1");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return;
  }

  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("activity_log", ImVec2(S(Metrics::LogW), 0),
                    ImGuiChildFlags_None);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();
  dl->AddLine(wp, ImVec2(wp.x, wp.y + ws.y), fw::theme::U32(kPalette.border));

  // Header (32 px)
  {
    ImGui::SetCursorPos(S2(8.f, 9.f));
    if (fw::ui::ChipBtn("\u25B7", false, BtnKind::Neutral))
    {
      log_collapsed = true;
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(S(11.f));
    Mono("LOG + CONSOLE", kPalette.text_dim, fs);
    if (has_error || poll_has_warn)
    {
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(S(11.f));
      fw::ui::StatusDot(3.f, kPalette.danger, false);
    }
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(S(11.f));
    char cnt[16];
    std::snprintf(cnt, sizeof(cnt), "%d",
                  static_cast<int>(active_view.size()));
    Mono(cnt, kPalette.muted, fs);
    ImGui::SameLine(ws.x - S(44.f));
    ImGui::SetCursorPosY(S(9.f));
    if (fw::ui::ChipBtn("CLR", false, BtnKind::Neutral))
    {
      if (log_panel_tab == 1)
      {
        poll_log.Clear();
      }
      else
      {
        log.Clear();
      }
    }
    dl->AddLine(ImVec2(wp.x, wp.y + S(32.f)),
                ImVec2(wp.x + ws.x, wp.y + S(32.f)),
                fw::theme::U32(kPalette.border));
    ImGui::SetCursorPosY(S(32.f));
  }

  // BUS | POLL tabs
  {
    ImGui::SetCursorPos(ImVec2(S(8.f), ImGui::GetCursorPosY() + S(4.f)));
    if (fw::ui::ChipBtn("BUS", log_panel_tab == 0, BtnKind::Neutral))
    {
      log_panel_tab = 0;
    }
    ImGui::SameLine(0.f, S(4.f));
    if (fw::ui::ChipBtn("POLL", log_panel_tab == 1, BtnKind::Neutral))
    {
      log_panel_tab = 1;
    }
    if (poll_has_warn)
    {
      ImGui::SameLine(0.f, S(6.f));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.f));
      fw::ui::StatusDot(3.f, kPalette.danger, false);
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.f));
    const float y = wp.y + ImGui::GetCursorPosY();
    dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ws.x, y),
                fw::theme::U32(kPalette.border));
  }

  // Filter
  {
    ImGui::SetCursorPos(ImVec2(S(8.f), ImGui::GetCursorPosY() + S(6.f)));
    ImGui::SetNextItemWidth(ws.x - S(16.f));
    ImGui::PushFont(fm);
    ImGui::InputTextWithHint("##logfilter",
                             log_panel_tab == 1 ? "filter poll…"
                                                : "filter log…",
                             log_filter, sizeof(log_filter));
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.f));
    const float y = wp.y + ImGui::GetCursorPosY();
    dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ws.x, y),
                fw::theme::U32(kPalette.border));
  }

  // Console only on BUS tab (target tabs + input + quick sends)
  const float console_h = (log_panel_tab == 0) ? S(92.f) : S(8.f);
  const float entries_h =
      ImGui::GetContentRegionAvail().y - console_h;

  // Entries — oldest top, newest bottom.
  ImGui::BeginChild("logscroll", ImVec2(0, entries_h), ImGuiChildFlags_None);
  for (const auto &e : active_view)
  {
    if (log_filter[0] && e.text.find(log_filter) == std::string::npos)
    {
      continue;
    }
    ImGui::PushID(&e);
    ImGui::SetCursorPosX(S(8.f));
    ImGui::BeginGroup();
    Mono(e.ts, kPalette.muted, fs);
    ImGui::SameLine(0.f, S(6.f));
    Mono(KindLabel(e.kind), KindColor(e.kind), fs);
    ImGui::SetCursorPosX(S(8.f));
    ImGui::PushFont(fm);
    ImGui::PushTextWrapPos(ws.x - S(8.f));
    ImGui::TextColored(e.kind == LogKind::Err ? kPalette.danger
                                              : kPalette.text,
                       "%s", e.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Click to copy");
    }
    if (ImGui::IsItemClicked())
    {
      ImGui::SetClipboardText(e.text.c_str());
      PushToastOk("copied");
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (active_view.empty())
  {
    ImGui::Dummy(ImVec2(0, S(16.f)));
    const char *empty_msg =
        (log_panel_tab == 1) ? "no poll events" : "no entries";
    const float tw =
        fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, empty_msg).x;
    ImGui::SetCursorPosX((ws.x - tw) * 0.5f);
    Mono(empty_msg, kPalette.muted, fs);
  }
  if (log_auto_scroll &&
      ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 24.f)
  {
    ImGui::SetScrollHereY(1.f);
  }
  ImGui::EndChild();

  // ── Console command line (BUS tab only)
  const bool bus_online = bus.IsOpen() && !bus.BusFault();
  if (log_panel_tab == 0)
  {
    const float top = wp.y + ws.y - console_h;
    dl->AddLine(ImVec2(wp.x, top), ImVec2(wp.x + ws.x, top),
                fw::theme::U32(kPalette.border_hi));

    // Target tabs CH / EF / ALL
    static const char *kTargets[] = {"CH", "EF", "ALL"};
    const float tab_w = ws.x / 3.f;
    const float tab_h = S(18.f);
    for (int i = 0; i < 3; ++i)
    {
      const bool sel = (raw_target == i);
      const ImVec2 p0(wp.x + tab_w * static_cast<float>(i), top + 1.f);
      const ImVec2 p1(p0.x + tab_w, top + 1.f + tab_h);
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID(i);
      ImGui::InvisibleButton("##target", ImVec2(tab_w, tab_h));
      if (ImGui::IsItemClicked())
      {
        raw_target = i;
      }
      ImGui::PopID();
      if (sel)
      {
        dl->AddRectFilled(p0, p1, fw::theme::U32A(kPalette.accent, 0.08f));
        dl->AddLine(ImVec2(p0.x, p1.y - 1.f), ImVec2(p1.x, p1.y - 1.f),
                    fw::theme::U32(kPalette.accent));
      }
      const ImVec2 ts =
          fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, kTargets[i]);
      dl->AddText(fs, fs->FontSize,
                  ImVec2(p0.x + (tab_w - ts.x) * 0.5f,
                         p0.y + (tab_h - ts.y) * 0.5f),
                  fw::theme::U32(sel ? kPalette.accent : kPalette.muted),
                  kTargets[i]);
    }
    dl->AddLine(ImVec2(wp.x, top + 1.f + tab_h),
                ImVec2(wp.x + ws.x, top + 1.f + tab_h),
                fw::theme::U32(kPalette.border));

    // Prompt + input + send
    ImGui::SetCursorScreenPos(ImVec2(wp.x + S(8.f), top + tab_h + S(8.f)));
    char prompt[4];
    std::snprintf(prompt, sizeof(prompt), "%c>",
                  raw_target == 1 ? 'E' : raw_target == 2 ? 'A'
                                                          : 'C');
    Mono(prompt, kPalette.accent, fm);
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetNextItemWidth(ws.x - S(8.f + 24.f + 6.f + 34.f + 8.f));
    ImGui::BeginDisabled(!bus_online);
    ImGui::PushFont(fm);
    const bool enter = ImGui::InputTextWithHint(
        "##cmd", bus_online ? "\u2191\u2193 history · Enter send" : "bus offline",
        raw_cmd, sizeof(raw_cmd), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopFont();
    if (ImGui::IsItemActive())
    {
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !lab_history.empty())
      {
        if (lab_history_index < 0)
        {
          lab_history_index = static_cast<int>(lab_history.size()) - 1;
        }
        else if (lab_history_index > 0)
        {
          --lab_history_index;
        }
        std::snprintf(raw_cmd, sizeof(raw_cmd), "%s",
                      lab_history[static_cast<std::size_t>(lab_history_index)]
                          .c_str());
      }
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && lab_history_index >= 0)
      {
        if (lab_history_index + 1 < static_cast<int>(lab_history.size()))
        {
          ++lab_history_index;
          std::snprintf(raw_cmd, sizeof(raw_cmd), "%s",
                        lab_history[static_cast<std::size_t>(lab_history_index)]
                            .c_str());
        }
        else
        {
          lab_history_index = -1;
          raw_cmd[0] = '\0';
        }
      }
    }
    ImGui::SameLine(0.f, S(6.f));
    const bool send =
        fw::ui::Btn("\u23CE", ImVec2(S(34.f), 0), BtnKind::Primary) || enter;
    ImGui::EndDisabled();
    if (send && raw_cmd[0])
    {
      SendConsole(raw_cmd);
      raw_cmd[0] = '\0';
    }

    // Quick sends (real card commands)
    ImGui::SetCursorScreenPos(
        ImVec2(wp.x + S(8.f), top + tab_h + S(42.f)));
    ImGui::BeginDisabled(!bus_online);
    static const char *kQuick[] = {"h", "cpu 0", "cpu 4", "s", "u"};
    for (std::size_t i = 0; i < sizeof(kQuick) / sizeof(kQuick[0]); ++i)
    {
      if (i)
      {
        ImGui::SameLine(0.f, S(4.f));
      }
      if (fw::ui::ChipBtn(kQuick[i], false, BtnKind::Neutral))
      {
        SendConsole(kQuick[i]);
      }
    }
    ImGui::EndDisabled();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

/* ── Perform: hero scope + voice grid + piano ────────────────────────── */

void App::DrawPerform()
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;

  const float strip_h = S(272.f);
  const float ctrl_h = S(38.f);
  const float scope_h =
      ImGui::GetContentRegionAvail().y - strip_h;

  // Scope canvas (edge-to-edge, no padding — design fills the whole area)
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("scope_area", ImVec2(0, scope_h), ImGuiChildFlags_None);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  {
    const float wave_h = ImGui::GetContentRegionAvail().y - ctrl_h;
    const ImVec2 wave_p0 = ImGui::GetCursorScreenPos();
    const float sc = 1.25f * scope_volt;
    fw::ui::GlowWaveform("scope_wave", preview.Samples().data(),
                         PreviewScope::kDisplaySamples, ImVec2(-1, wave_h),
                         -sc, sc);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float wave_w = ImGui::GetItemRectSize().x;

    // Top-left overlay
    dl->AddText(fs, fs->FontSize,
                ImVec2(wave_p0.x + S(12.f), wave_p0.y + S(8.f)),
                fw::theme::U32A(kPalette.accent, 0.5f),
                "LOCAL PREVIEW / SCOPE");
    char sub[48];
    const int ac = preview.ActiveCount();
    std::snprintf(sub, sizeof(sub), "48 kHz · %d active voice%s", ac,
                  ac != 1 ? "s" : "");
    dl->AddText(fs, fs->FontSize,
                ImVec2(wave_p0.x + S(12.f), wave_p0.y + S(21.f)),
                fw::theme::U32(kPalette.muted), sub);

    // Top-right readouts
    char tb[24];
    FormatTimebase(scope_time_ms, tb, sizeof(tb));
    char tb_div[32];
    std::snprintf(tb_div, sizeof(tb_div), "%s/div", tb);
    ImVec2 ts = fm->CalcTextSizeA(fm->FontSize, FLT_MAX, 0.f, tb_div);
    dl->AddText(fm, fm->FontSize,
                ImVec2(wave_p0.x + wave_w - ts.x - S(12.f),
                       wave_p0.y + S(8.f)),
                fw::theme::U32(kPalette.accent), tb_div);
    char vd[24];
    std::snprintf(vd, sizeof(vd), "%.1f\u00D7V/div",
                  static_cast<double>(scope_volt));
    ts = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, vd);
    dl->AddText(fs, fs->FontSize,
                ImVec2(wave_p0.x + wave_w - ts.x - S(12.f),
                       wave_p0.y + S(23.f)),
                fw::theme::U32(kPalette.text_dim), vd);

    // Controls bar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.f), 0.f));
    ImGui::BeginChild("scope_ctrl", ImVec2(0, ctrl_h), ImGuiChildFlags_None);
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const ImVec2 cwp = ImGui::GetWindowPos();
    const ImVec2 cws = ImGui::GetWindowSize();
    cdl->AddLine(cwp, ImVec2(cwp.x + cws.x, cwp.y),
                 fw::theme::U32(kPalette.border));

    const float mid_y = (ctrl_h - S(18.f)) * 0.5f;
    ImGui::SetCursorPos(ImVec2(S(16.f), mid_y + S(3.f)));
    Mono("TIME/DIV", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y);
    if (fw::ui::ChipBtn("\u2212", false, BtnKind::Neutral))
    {
      scope_time_ms = std::max(0.1f, scope_time_ms * 0.5f);
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(mid_y + S(2.f));
    {
      char tbuf[24];
      FormatTimebase(scope_time_ms, tbuf, sizeof(tbuf));
      const float vw = S(44.f);
      const float tw = fm->CalcTextSizeA(fm->FontSize, FLT_MAX, 0.f, tbuf).x;
      ImGui::Dummy(ImVec2(vw, S(14.f)));
      cdl->AddText(fm, fm->FontSize,
                   ImVec2(ImGui::GetItemRectMin().x + (vw - tw) * 0.5f,
                          ImGui::GetItemRectMin().y),
                   fw::theme::U32(kPalette.accent), tbuf);
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(mid_y);
    if (fw::ui::ChipBtn("+", false, BtnKind::Neutral))
    {
      scope_time_ms = std::min(200.f, scope_time_ms * 2.f);
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(1.f));
    ImGui::SetNextItemWidth(S(70.f));
    if (ImGui::SliderFloat("##tbslide", &scope_time_ms, 0.1f, 200.f, "",
                           ImGuiSliderFlags_Logarithmic))
    {
      MarkSettingsDirty();
    }

    ImGui::SameLine(0.f, S(18.f));
    ImGui::SetCursorPosY(mid_y + S(3.f));
    Mono("VOLTS/DIV", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y);
    if (fw::ui::ChipBtn("\u2212##v", false, BtnKind::Neutral))
    {
      scope_volt = std::max(0.1f, scope_volt / 1.5f);
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(mid_y + S(2.f));
    {
      char vbuf[16];
      std::snprintf(vbuf, sizeof(vbuf), "%.2f",
                    static_cast<double>(scope_volt));
      const float vw = S(32.f);
      const float tw = fm->CalcTextSizeA(fm->FontSize, FLT_MAX, 0.f, vbuf).x;
      ImGui::Dummy(ImVec2(vw, S(14.f)));
      cdl->AddText(fm, fm->FontSize,
                   ImVec2(ImGui::GetItemRectMin().x + (vw - tw) * 0.5f,
                          ImGui::GetItemRectMin().y),
                   fw::theme::U32(kPalette.accent), vbuf);
    }
    ImGui::SameLine(0.f, S(6.f));
    ImGui::SetCursorPosY(mid_y);
    if (fw::ui::ChipBtn("+##v", false, BtnKind::Neutral))
    {
      scope_volt = std::min(8.f, scope_volt * 1.5f);
      MarkSettingsDirty();
    }
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY(mid_y + S(1.f));
    ImGui::SetNextItemWidth(S(70.f));
    if (ImGui::SliderFloat("##vslide", &scope_volt, 0.1f, 8.f, ""))
    {
      MarkSettingsDirty();
    }

    // Divider
    ImGui::SameLine(0.f, S(14.f));
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      cdl->AddLine(ImVec2(p.x, cwp.y + S(11.f)), ImVec2(p.x, cwp.y + S(27.f)),
                   fw::theme::U32(kPalette.border));
      ImGui::Dummy(ImVec2(1.f, 0.f));
    }

    // Readouts
    struct Readout
    {
      const char *label;
      float value;
      bool is_ms;
    };
    const Readout reads[] = {
        {"PEAK", preview.Peak(), false},
        {"RMS", preview.Rms(), false},
        {"P-P", preview.PkPk(), false},
        {"WIN", scope_time_ms * 10.f, true},
    };
    const bool any_active = preview.ActiveCount() > 0;
    for (const auto &r : reads)
    {
      ImGui::SameLine(0.f, S(14.f));
      ImGui::SetCursorPosY(mid_y + S(2.f));
      Mono(r.label, kPalette.text_dim, fs);
      ImGui::SameLine(0.f, S(4.f));
      ImGui::SetCursorPosY(mid_y + S(2.f));
      char v[24];
      if (r.is_ms)
      {
        std::snprintf(v, sizeof(v), "%.1fms", static_cast<double>(r.value));
      }
      else
      {
        std::snprintf(v, sizeof(v), "%.3f", static_cast<double>(r.value));
      }
      Mono(v, any_active ? kPalette.text : kPalette.muted, fs);
    }

    // Level meter, right aligned
    ImGui::SameLine(
        std::max(ImGui::GetCursorPosX() + S(10.f), cws.x - S(52.f)));
    ImGui::SetCursorPosY(mid_y + S(2.f));
    Mono("LVL", kPalette.muted, fs);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::SetCursorPosY((ctrl_h - S(24.f)) * 0.5f);
    fw::ui::VerticalMeter("lvl", preview.Peak(), S2(6.f, 24.f), peak_hold);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }
  ImGui::PopStyleVar(); // scope_area ItemSpacing
  ImGui::EndChild();

  // ── Bottom strip: voice grid + piano
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("perform_strip", ImVec2(0, strip_h), ImGuiChildFlags_None);
  ImDrawList *sdl = ImGui::GetWindowDrawList();
  {
    const ImVec2 swp = ImGui::GetWindowPos();
    const ImVec2 sws = ImGui::GetWindowSize();
    sdl->AddLine(swp, ImVec2(swp.x + sws.x, swp.y),
                 fw::theme::U32(kPalette.border));
  }

  const auto &slots = bank.Slots();
  int active_count = 0;
  for (const auto &s : slots)
  {
    if (s.active)
    {
      ++active_count;
    }
  }

  // Voice grid (224 px logical)
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
  ImGui::BeginChild("voice_grid", ImVec2(S(224.f), 0), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + wsz.x - 1.f, wp.y),
                ImVec2(wp.x + wsz.x - 1.f, wp.y + wsz.y),
                fw::theme::U32(kPalette.border));

    ImGui::SetCursorPos(S2(12.f, 8.f));
    Mono("VOICES n0–nf", kPalette.text_dim, fs);
    char ac[24];
    std::snprintf(ac, sizeof(ac), "%d active", active_count);
    const float aw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, ac).x;
    ImGui::SameLine(wsz.x - aw - S(12.f));
    ImGui::SetCursorPosY(S(8.f));
    Mono(ac, kPalette.accent, fs);
    dl->AddLine(ImVec2(wp.x, wp.y + S(26.f)),
                ImVec2(wp.x + wsz.x, wp.y + S(26.f)),
                fw::theme::U32(kPalette.border));

    // 4×2 grid — id / note / Hz when active
    const float gap = S(4.f);
    const float pad = S(8.f);
    const int nvoices = static_cast<int>(cardlink::midi::kVoiceCount);
    const int ncols = 4;
    const int nrows = (nvoices + ncols - 1) / ncols;
    const float cell_w = (wsz.x - pad * 2.f - gap * static_cast<float>(ncols - 1)) /
                         static_cast<float>(ncols);
    const float cell_h = S(38.f);
    ImFont *fcap = fw::theme::g_fonts.caps;
    for (int i = 0; i < nvoices; ++i)
    {
      const int row = i / ncols;
      const int col = i % ncols;
      const ImVec2 p0(wp.x + pad + static_cast<float>(col) * (cell_w + gap),
                      wp.y + S(32.f) +
                          static_cast<float>(row) * (cell_h + gap));
      const ImVec2 p1(p0.x + cell_w, p0.y + cell_h);
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID(i);
      ImGui::InvisibleButton("##v", ImVec2(cell_w, cell_h));
      if (ImGui::IsItemClicked())
      {
        selected_voice = i;
      }
      ImGui::PopID();
      const auto &s = slots[static_cast<std::size_t>(i)];
      const bool sel = (selected_voice == i);
      ImVec4 bg = kPalette.bg_alt;
      if (s.active)
      {
        bg = WithA(kPalette.accent, 0.12f);
      }
      else if (sel)
      {
        bg = kPalette.panel_alt;
      }
      dl->AddRectFilled(p0, p1, fw::theme::U32(bg), S(2.f));
      dl->AddRect(p0, p1,
                  fw::theme::U32(sel        ? WithA(kPalette.accent, 0.35f)
                                 : s.active ? WithA(kPalette.accent, 0.18f)
                                            : kPalette.border),
                  S(2.f));
      char id[4];
      std::snprintf(id, sizeof(id), "n%x", i);
      const ImVec2 idw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, id);
      dl->AddText(fs, fs->FontSize,
                  ImVec2(p0.x + (cell_w - idw.x) * 0.5f, p0.y + S(2.f)),
                  fw::theme::U32(s.active ? kPalette.accent : kPalette.muted),
                  id);
      if (s.active)
      {
        const std::string note = cardlink::midi::MidiNoteName(s.midi_key);
        const ImVec2 nw =
            fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, note.c_str());
        dl->AddText(fs, fs->FontSize,
                    ImVec2(p0.x + (cell_w - nw.x) * 0.5f, p0.y + S(13.f)),
                    fw::theme::U32(kPalette.accent_bright), note.c_str());
        char hz[12];
        if (s.freq_hz >= 1000.0)
        {
          std::snprintf(hz, sizeof(hz), "%.2fk", s.freq_hz / 1000.0);
        }
        else if (s.freq_hz >= 100.0)
        {
          std::snprintf(hz, sizeof(hz), "%.0f", s.freq_hz);
        }
        else
        {
          std::snprintf(hz, sizeof(hz), "%.1f", s.freq_hz);
        }
        const ImVec2 hw =
            fcap->CalcTextSizeA(fcap->FontSize, FLT_MAX, 0.f, hz);
        dl->AddText(fcap, fcap->FontSize,
                    ImVec2(p0.x + (cell_w - hw.x) * 0.5f, p0.y + S(24.f)),
                    fw::theme::U32(kPalette.text_dim), hz);
      }
      else
      {
        const ImVec2 dw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, "·");
        dl->AddText(fs, fs->FontSize,
                    ImVec2(p0.x + (cell_w - dw.x) * 0.5f, p0.y + S(16.f)),
                    fw::theme::U32(kPalette.muted), "·");
      }
    }

    // Selected voice footer
    if (selected_voice >= 0 &&
        selected_voice < static_cast<int>(cardlink::midi::kVoiceCount))
    {
      const float fy =
          wp.y + S(32.f) + static_cast<float>(nrows) * (cell_h + gap) + S(2.f);
      dl->AddLine(ImVec2(wp.x, fy), ImVec2(wp.x + wsz.x, fy),
                  fw::theme::U32(kPalette.border));
      ImGui::SetCursorScreenPos(ImVec2(wp.x + S(12.f), fy + S(6.f)));
      const auto &s = slots[static_cast<std::size_t>(selected_voice)];
      char info[64];
      if (s.active)
      {
        std::snprintf(info, sizeof(info), "n%x — %s %.2f Hz", selected_voice,
                      cardlink::midi::MidiNoteName(s.midi_key).c_str(),
                      static_cast<double>(s.freq_hz));
      }
      else
      {
        std::snprintf(info, sizeof(info), "n%x — silent", selected_voice);
      }
      Mono(info, kPalette.text_dim, fs);
      ImGui::SetCursorPosX(S(12.f));
      if (fw::ui::ChipBtn("\u2192 Tone", false, BtnKind::Neutral))
      {
        view = GuiView::Tone;
        MarkSettingsDirty();
      }
      ImGui::SameLine(0.f, S(6.f));
      if (fw::ui::ChipBtn("\u2192 Sample", false, BtnKind::Neutral))
      {
        view = GuiView::Sample;
        MarkSettingsDirty();
      }

      double c4_units = 0.0;
      double samp_ms = 0.0;
      SumUsbBodyLoad(bank, samples.Mixer(), c4_units, samp_ms);
      const bool over = samp_ms > kUsbBodySampPerMs;
      const ImVec4 load_col = over ? kPalette.danger : kPalette.accent;
      ImGui::SetCursorPosX(S(12.f));
      ImGui::Dummy(ImVec2(1.f, S(6.f)));
      ImGui::SetCursorPosX(S(12.f));
      Mono("USB BODY", kPalette.text_dim, fs);
      char load[48];
      std::snprintf(load, sizeof(load), "%.0f/%.0f /ms", samp_ms,
                    kUsbBodySampPerMs);
      ImGui::SetCursorPosX(S(12.f));
      Mono(load, load_col, fm);
      char units[48];
      std::snprintf(units, sizeof(units), "%.2f\u00D7C4  %s", c4_units,
                    over ? "OVER" : "OK");
      ImGui::SetCursorPosX(S(12.f));
      Mono(units, over ? kPalette.danger : kPalette.text_dim, fs);
      {
        const float bar_w = wsz.x - S(24.f);
        const float bar_h = S(4.f);
        ImGui::SetCursorPosX(S(12.f));
        ImGui::InvisibleButton("##usb_body_load", ImVec2(bar_w, bar_h + S(8.f)));
        const ImVec2 bp0 = ImGui::GetItemRectMin();
        const ImVec2 bp1(bp0.x + bar_w, bp0.y + bar_h);
        const float frac = static_cast<float>(
            std::min(1.0, samp_ms / kUsbBodySampPerMs));
        dl->AddRectFilled(bp0, bp1, fw::theme::U32(kPalette.bg), S(1.f));
        if (frac > 0.f)
        {
          dl->AddRectFilled(bp0,
                            ImVec2(bp0.x + bar_w * frac, bp1.y),
                            fw::theme::U32(load_col), S(1.f));
        }
        dl->AddRect(bp0, bp1, fw::theme::U32(kPalette.border), S(1.f));
        if (ImGui::IsItemHovered())
        {
          ImGui::BeginTooltip();
          ImGui::PushFont(fs);
          ImGui::Text(
              "Each voice consumes 48 \u00D7 (note Hz / root Hz) samples/ms.\n"
              "BODY is vendor bulk: one permitted packed transfer may span\n"
              "two FS frames (up to 2432 bytes). Every voice shares it.\n"
              "%.0f samp/ms is one voice; %.0f is five voices\n"
              "(5\u00D7C5 = 480). 8\u00D7C4 = 384.",
              kUsbBodySampPerMsWire, kUsbBodySampPerMs);
          ImGui::PopFont();
          ImGui::EndTooltip();
        }
      }
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Piano
  ImGui::SameLine(0.f, 0.f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
  ImGui::BeginChild("piano_area", ImVec2(0, 0), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wsz = ImGui::GetWindowSize();

    const int oct = 4 + piano_octave; // display octave (design Oct 0-7)
    const float WW = S(28.f);
    const float WH = S(70.f);
    const float BW = S(18.f);
    const float BH = S(44.f);
    constexpr int kWhites = 14;
    const float piano_w = kWhites * WW;

    // Octave controls (centered)
    {
      const float row_w = S(60.f + 8.f + 44.f + 8.f + 60.f + 10.f + 34.f);
      ImGui::SetCursorPos(
          ImVec2(std::max(S(8.f), (wsz.x - row_w) * 0.5f), S(14.f)));
      ImGui::BeginDisabled(oct <= 0);
      if (fw::ui::ChipBtn("\u25C0 Oct", false, BtnKind::Neutral))
      {
        piano_octave = std::max(piano_octave - 1, -4);
        MarkSettingsDirty();
      }
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, S(8.f));
      char ob[16];
      std::snprintf(ob, sizeof(ob), "Oct %d", oct);
      ImGui::SetCursorPosY(S(16.f));
      Mono(ob, kPalette.accent, fm);
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(S(14.f));
      ImGui::BeginDisabled(oct >= 7);
      if (fw::ui::ChipBtn("Oct \u25B6", false, BtnKind::Neutral))
      {
        piano_octave = std::min(piano_octave + 1, 3);
        MarkSettingsDirty();
      }
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, S(10.f));
      ImGui::SetCursorPosY(S(16.f));
      Mono("[Z/X]", kPalette.muted, fs);
    }

    const float px0 = std::max(S(8.f), (wsz.x - piano_w) * 0.5f);
    const float py0 = S(44.f);
    const ImVec2 origin =
        ImVec2(ImGui::GetWindowPos().x + px0, ImGui::GetWindowPos().y + py0);

    const int base_midi = 12 + oct * 12; // C of displayed octave

    auto NoteActive = [&](int midi_key)
    {
      for (const auto &s : slots)
      {
        if (s.active && s.midi_key == midi_key)
        {
          return true;
        }
      }
      return false;
    };
    auto NoteOnOff = [&](int midi_key, bool on)
    {
      if (midi_key < 0 || midi_key > 127)
      {
        return;
      }
      if (on)
      {
        ApplyBankEvents(bank.NoteOn(static_cast<uint8_t>(midi_key)));
      }
      else
      {
        ApplyBankEvents(bank.NoteOff(static_cast<uint8_t>(midi_key)));
      }
    };

    static const int kWhiteSemis[7] = {0, 2, 4, 5, 7, 9, 11};
    static const char *kWhiteNames[7] = {"C", "D", "E", "F", "G", "A", "B"};
    static const char *kWhiteKeys[7] = {"a", "s", "d", "f", "g", "h", "j"};

    // White keys
    for (int wi = 0; wi < kWhites; ++wi)
    {
      const int oct_off = wi / 7;
      const int ni = wi % 7;
      const int midi_key = base_midi + oct_off * 12 + kWhiteSemis[ni];
      const bool sounding = NoteActive(midi_key);
      const ImVec2 p0(origin.x + static_cast<float>(wi) * WW, origin.y);
      const ImVec2 p1(p0.x + WW - 1.f, p0.y + WH);
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID(100 + wi);
      ImGui::InvisibleButton("##wk", ImVec2(WW - 1.f, WH));
      if (ImGui::IsItemActivated())
      {
        NoteOnOff(midi_key, true);
      }
      if (ImGui::IsItemDeactivated())
      {
        NoteOnOff(midi_key, false);
      }
      ImGui::PopID();
      const ImVec4 fill = sounding
                              ? WithA(kPalette.accent, 0.85f)
                              : ImVec4(0.784f, 0.878f, 0.784f, 0.92f);
      dl->AddRectFilled(p0, p1, fw::theme::U32(fill), S(3.f),
                        ImDrawFlags_RoundCornersBottom);
      dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 90), S(3.f),
                  ImDrawFlags_RoundCornersBottom);
      if (sounding)
      {
        dl->AddRect(p0, p1, fw::theme::U32A(kPalette.accent, 0.6f), S(3.f),
                    ImDrawFlags_RoundCornersBottom, 2.f);
      }
      const char *lbl = "";
      if (oct_off == 0)
      {
        lbl = kWhiteKeys[ni];
      }
      else if (ni == 0)
      {
        lbl = "k";
      }
      else
      {
        lbl = kWhiteNames[ni];
      }
      const ImVec2 lsz = fs->CalcTextSizeA(fs->FontSize - 1.f, FLT_MAX, 0.f, lbl);
      dl->AddText(fs, fs->FontSize - 1.f,
                  ImVec2(p0.x + (WW - 1.f - lsz.x) * 0.5f, p1.y - S(13.f)),
                  sounding ? IM_COL32(7, 9, 8, 255) : IM_COL32(0, 0, 0, 110),
                  lbl);
    }

    // Black keys (submitted after whites so they win hover)
    struct BlackDef
    {
      float offset;
      int semi;
      const char *key;
    };
    static const BlackDef kBlacks[5] = {
        {0.65f, 1, "w"},
        {1.65f, 3, "e"},
        {3.65f, 6, "t"},
        {4.65f, 8, "y"},
        {5.65f, 10, "u"},
    };
    for (int oct_off = 0; oct_off < 2; ++oct_off)
    {
      for (int bi = 0; bi < 5; ++bi)
      {
        const BlackDef &b = kBlacks[bi];
        const int midi_key = base_midi + oct_off * 12 + b.semi;
        const bool sounding = NoteActive(midi_key);
        const float left =
            origin.x + (static_cast<float>(oct_off) * 7.f + b.offset) * WW;
        const ImVec2 p0(left, origin.y);
        const ImVec2 p1(left + BW, origin.y + BH);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(200 + oct_off * 5 + bi);
        ImGui::InvisibleButton("##bk", ImVec2(BW, BH));
        if (ImGui::IsItemActivated())
        {
          NoteOnOff(midi_key, true);
        }
        if (ImGui::IsItemDeactivated())
        {
          NoteOnOff(midi_key, false);
        }
        ImGui::PopID();
        const ImVec4 fill =
            sounding ? kPalette.accent : ImVec4(0.059f, 0.090f, 0.059f, 1.f);
        dl->AddRectFilled(p0, p1, fw::theme::U32(fill), S(3.f),
                          ImDrawFlags_RoundCornersBottom);
        dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 150), S(3.f),
                    ImDrawFlags_RoundCornersBottom);
        const char *lbl = (oct_off == 0) ? b.key : "";
        if (lbl[0])
        {
          const ImVec2 lsz =
              fs->CalcTextSizeA(fs->FontSize - 2.f, FLT_MAX, 0.f, lbl);
          dl->AddText(fs, fs->FontSize - 2.f,
                      ImVec2(p0.x + (BW - lsz.x) * 0.5f, p1.y - S(12.f)),
                      sounding ? IM_COL32(7, 9, 8, 255)
                               : IM_COL32(255, 255, 255, 80),
                      lbl);
        }
      }
    }

    // Hint
    {
      const char *hint = "Click keys or use keyboard · Z=oct\u2212 · X=oct+";
      const float hw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, hint).x;
      ImGui::SetCursorPos(ImVec2(std::max(S(8.f), (wsz.x - hw) * 0.5f),
                                 py0 + WH + S(10.f)));
      Mono(hint, kPalette.muted, fs);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::EndChild(); // perform_strip
  ImGui::PopStyleVar();
}

/* ── Tone: mode bar + oscillator / filter + envelope ─────────────────── */

void App::DrawTone()
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;
  const bool offline = !bus.IsOpen() || bus.BusFault();

  // SAMPLE path label (no mode switch — this branch is SAMPLE-only)
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.f), 0.f));
  ImGui::BeginChild("mode_bar", ImVec2(0, S(40.f)), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x, wp.y + wsz.y - 1.f),
                ImVec2(wp.x + wsz.x, wp.y + wsz.y - 1.f),
                fw::theme::U32(kPalette.border));
    const float mid_y = S(10.f);
    ImGui::SetCursorPos(ImVec2(S(16.f), mid_y + S(2.f)));
    Mono("PATH", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(12.f));
    ImGui::SetCursorPosY(mid_y);
    Mono("SAMPLE", kPalette.accent, fm);
    if (offline)
    {
      ImGui::SameLine(0.f, S(18.f));
      ImGui::SetCursorPosY(mid_y + S(5.f));
      fw::ui::StatusDot(3.f, kPalette.muted, false);
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(mid_y + S(3.f));
      Mono("Bus offline —", kPalette.text_dim, fs);
      ImGui::SameLine(0.f, S(4.f));
      ImGui::SetCursorPosY(mid_y + S(2.f));
      if (LinkText("open Setup"))
      {
        view = GuiView::Setup;
        MarkSettingsDirty();
      }
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Columns
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(12.f, 12.f));
  ImGui::BeginChild("tone_body", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGui::PopStyleVar();

  ImGui::BeginChild("tone_left", ImVec2(S(280.f), 0), ImGuiChildFlags_None);
  DrawOscillatorCard(*this);
  ImGui::Spacing();
  DrawFilterCard(*this);
  ImGui::EndChild();

  ImGui::SameLine(0.f, S(12.f));
  ImGui::BeginChild("tone_right", ImVec2(0, 0), ImGuiChildFlags_None);
  DrawEnvelopeEditor(*this);
  ImGui::EndChild();

  ImGui::EndChild();
}

/* ── Setup ───────────────────────────────────────────────────────────── */

namespace
{
  /** Design section header row: surface bg, bottom border, caps title. */
  void SetupHeader(const char *title)
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowSize().x;
    const float h = S(32.f);
    const ImVec2 p0(wp.x, ImGui::GetCursorScreenPos().y);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                      fw::theme::U32(kPalette.bg_alt));
    dl->AddLine(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + w, p0.y + h),
                fw::theme::U32(kPalette.border));
    ImFont *f = fw::theme::g_fonts.mono_small;
    const float ty = p0.y + (h - f->FontSize) * 0.5f;
    dl->AddText(f, f->FontSize, ImVec2(p0.x + S(16.f), ty),
                fw::theme::U32(kPalette.text_dim), title);
    // Height-only: full-width Dummy can force a horizontal scrollbar.
    ImGui::Dummy(ImVec2(0.f, h));
  }

  /** Borderless children ignore WindowPadding unless this flag is set. */
  constexpr ImGuiChildFlags kSetupBodyChild =
      ImGuiChildFlags_AlwaysUseWindowPadding;
} // namespace

void App::DrawSetup()
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;

  const bool is_online = bus.IsOpen() && !bus.BusFault();
  const bool is_fault = bus.IsOpen() && bus.BusFault();
  const bool is_connecting = bus.IsConnecting();

  // Edge-to-edge columns; section bodies apply their own padding.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("setup_left", ImVec2(S(360.f), 0), ImGuiChildFlags_None);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 wsz = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + wsz.x - 1.f, wp.y),
                ImVec2(wp.x + wsz.x - 1.f, wp.y + wsz.y),
                fw::theme::U32(kPalette.border));
  }
  SetupHeader("RS485 BUS (921600 BAUD)");

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(16.f, 16.f));
  ImGui::BeginChild("rs485_body", ImVec2(0, 0), kSetupBodyChild);
  ImGui::PopStyleVar();

  // Status card
  {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
    ImGui::BeginChild("bus_status", ImVec2(0, S(38.f)),
                      ImGuiChildFlags_Borders);
    ImGui::SetCursorPos(S2(12.f, 11.f));
    fw::ui::StatusDot(4.f, BusColor(bus), bus.IsOpen() || is_fault);
    ImGui::SameLine(0.f, S(10.f));
    ImGui::SetCursorPosY(S(10.f));
    const ImVec4 scol = is_fault        ? kPalette.danger
                        : is_online     ? kPalette.accent
                        : is_connecting ? kPalette.warning
                                        : kPalette.text_dim;
    Mono(BusStatusText(bus), scol, fm);
    if (is_online)
    {
      ImGui::SameLine(0.f, S(10.f));
      ImGui::SetCursorPosY(S(12.f));
      Mono(bus.Path().c_str(), kPalette.text_dim, fs);
    }
    if (is_connecting)
    {
      ImGui::SameLine(0.f, S(10.f));
      ImGui::SetCursorPosY(S(12.f));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fw::theme::Blink01());
      Mono("connecting…", kPalette.warning, fs);
      ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();

  // Health counters
  if (is_online || is_fault)
  {
    const float half =
        (ImGui::GetContentRegionAvail().x - S(8.f)) * 0.5f;
    struct Counter
    {
      const char *label;
      uint32_t value;
    };
    const Counter counters[] = {
        {"Timeouts", bus.TimeoutCount()},
        {"Errors", bus.ErrCount()},
    };
    for (int i = 0; i < 2; ++i)
    {
      if (i)
      {
        ImGui::SameLine(0.f, S(8.f));
      }
      ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.bg_alt);
      ImGui::PushID(i);
      ImGui::BeginChild("counter", ImVec2(half, S(56.f)),
                        ImGuiChildFlags_Borders);
      ImGui::SetCursorPos(S2(12.f, 8.f));
      Mono(counters[i].label, kPalette.text_dim, fs);
      ImGui::SetCursorPosX(S(12.f));
      char v[16];
      std::snprintf(v, sizeof(v), "%u", counters[i].value);
      Mono(v, counters[i].value > 0 ? kPalette.danger : kPalette.accent,
           fw::theme::g_fonts.hero);
      ImGui::EndChild();
      ImGui::PopID();
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
  }

  // Port select
  Mono("Serial Port", kPalette.text_dim, fs);
  ImGui::Dummy(ImVec2(0, S(2.f)));
  {
    const float btn_w = S(30.f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_w -
                            S(8.f));
    ImGui::BeginDisabled(is_online || is_connecting);
    if (ImGui::BeginCombo("##rs485", serial_path_buf[0]
                                         ? serial_path_buf
                                         : "\u2014 select port \u2014"))
    {
      for (std::size_t i = 0; i < serial_ports.size(); ++i)
      {
        const bool sel =
            (std::strcmp(serial_path_buf, serial_ports[i].c_str()) == 0);
        if (ImGui::Selectable(serial_ports[i].c_str(), sel))
        {
          std::snprintf(serial_path_buf, sizeof(serial_path_buf), "%s",
                        serial_ports[i].c_str());
          serial_port_index = static_cast<int>(i);
          MarkSettingsDirty();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.f, S(8.f));
    if (fw::ui::RefreshBtn("port_refresh", ImVec2(btn_w, S(24.f))))
    {
      log.Push("Refreshing port list…");
      RefreshPortLists();
    }
    ImGui::BeginDisabled(is_online || is_connecting);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##path", "Or enter path manually…",
                             serial_path_buf, sizeof(serial_path_buf));
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
      MarkSettingsDirty();
    }
    ImGui::EndDisabled();
  }
  ImGui::Spacing();

  // Connect / disconnect
  if (!bus.IsOpen() && !is_connecting)
  {
    ImGui::BeginDisabled(serial_path_buf[0] == '\0');
    if (fw::ui::Btn("Connect", ImVec2(-1, S(30.f)), BtnKind::Primary))
    {
      RequestConnectBus();
    }
    ImGui::EndDisabled();
  }
  if (is_connecting)
  {
    if (fw::ui::Btn("Cancel", ImVec2(-1, S(30.f)), BtnKind::Warn))
    {
      DisconnectBus();
    }
  }
  if (is_online)
  {
    if (fw::ui::Btn("Disconnect", ImVec2(-1, S(30.f)), BtnKind::Danger))
    {
      DisconnectBus();
      AllNotesOff();
    }
  }
  if (is_fault)
  {
    if (fw::ui::Btn("Recover from Fault", ImVec2(-1, S(30.f)),
                    BtnKind::Danger))
    {
      bus.RequestRecover(log);
      log.Push("Bus fault — recover queued");
    }
  }
  ImGui::Spacing();

  if (ImGui::Checkbox("##autorc", &auto_reconnect))
  {
    MarkSettingsDirty();
  }
  ImGui::SameLine(0.f, S(8.f));
  Mono("Auto-reconnect on launch", kPalette.text_dim, fs);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  Mono("DEBUG", kPalette.muted, fs);
  ImGui::BeginDisabled(!is_online);
  if (fw::ui::ChipBtn("Ping", false, BtnKind::Neutral))
  {
    const auto r = bus.QueueExec(cardproto::Target::Channel, "h");
    NotifyEnqueue(r == BusQueueResult::Ok, QueueResultMsg(r));
  }
  ImGui::EndDisabled();

  ImGui::EndChild(); // rs485_body
  ImGui::EndChild(); // setup_left

  // Right column
  ImGui::SameLine(0.f, 0.f);
  ImGui::BeginChild("setup_right", ImVec2(0, 0), ImGuiChildFlags_None);

  // MIDI
  SetupHeader("MIDI");
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(16.f, 16.f));
  ImGui::BeginChild("midi_body", ImVec2(0, S(148.f)), kSetupBodyChild);
  ImGui::PopStyleVar();
  {
    fw::ui::StatusDot(4.f, midi_open ? kPalette.accent : kPalette.muted,
                      midi_open);
    ImGui::SameLine(0.f, S(10.f));
    Mono(midi_open ? "MIDI ON" : "MIDI OFF",
         midi_open ? kPalette.accent : kPalette.text_dim, fm);
    if (midi_open)
    {
      ImGui::SameLine(0.f, S(10.f));
      Mono(midi.PortName().c_str(), kPalette.text_dim, fs);
    }
    ImGui::Dummy(ImVec2(0, S(6.f)));
    Mono("MIDI Port", kPalette.text_dim, fs);
    ImGui::Dummy(ImVec2(0, S(2.f)));
    std::string midi_preview = "\u2014 auto-pick \u2014";
    if (midi_port_index >= 0 &&
        midi_port_index < static_cast<int>(midi_ports.size()))
    {
      midi_preview =
          midi_ports[static_cast<std::size_t>(midi_port_index)].name;
    }
    const float btn_w = S(30.f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_w -
                            S(8.f));
    ImGui::BeginDisabled(midi_open);
    if (ImGui::BeginCombo("##midiport", midi_preview.c_str()))
    {
      if (ImGui::Selectable("\u2014 auto-pick \u2014", midi_port_index < 0))
      {
        midi_port_index = -1;
        MarkSettingsDirty();
      }
      for (const auto &p : midi_ports)
      {
        if (ImGui::Selectable(p.name.c_str(),
                              midi_port_index == static_cast<int>(p.index)))
        {
          midi_port_index = static_cast<int>(p.index);
          MarkSettingsDirty();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.f, S(8.f));
    if (fw::ui::RefreshBtn("midi_refresh", ImVec2(btn_w, S(24.f))))
    {
      log.Push("Refreshing MIDI ports…");
      RefreshPortLists();
    }
    ImGui::Dummy(ImVec2(0, S(6.f)));
    if (!midi_open)
    {
      if (fw::ui::Btn("Open MIDI", S2(120.f, 26.f), BtnKind::Primary))
      {
        ConnectMidi();
      }
    }
    else
    {
      if (fw::ui::Btn("Close MIDI", S2(120.f, 26.f), BtnKind::Danger))
      {
        DisconnectMidi();
      }
    }
  }
  ImGui::EndChild();

  // Output routing
  SetupHeader("OUTPUT ROUTING");
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(16.f, 16.f));
  ImGui::BeginChild("out_body", ImVec2(0, S(168.f)), kSetupBodyChild);
  ImGui::PopStyleVar();
  {
    struct OutOpt
    {
      OutMode mode;
      const char *label;
      const char *desc;
    };
    static const OutOpt kOpts[] = {
        {OutMode::Speakers, "Speakers", "Local host audio only"},
        {OutMode::Card, "Card", "Notes \u2192 hardware only"},
        {OutMode::Both, "Both", "Local preview + hardware"},
    };
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (const auto &o : kOpts)
    {
      const bool sel = (out_mode == o.mode);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      const float h = S(40.f);
      ImGui::PushID(o.label);
      ImGui::InvisibleButton("##opt", ImVec2(w, h));
      const bool clicked = ImGui::IsItemClicked();
      ImGui::PopID();
      const ImVec2 p1(p0.x + w, p0.y + h);
      if (sel)
      {
        dl->AddRectFilled(p0, p1, fw::theme::U32A(kPalette.accent, 0.05f),
                          S(2.f));
      }
      dl->AddRect(p0, p1,
                  fw::theme::U32(sel ? WithA(kPalette.accent, 0.30f)
                                     : kPalette.border),
                  S(2.f));
      // radio circle
      const ImVec2 rc(p0.x + S(16.f), p0.y + h * 0.5f);
      dl->AddCircle(rc, S(6.f),
                    fw::theme::U32(sel ? kPalette.accent
                                       : kPalette.border_hi),
                    16, 2.f);
      if (sel)
      {
        dl->AddCircleFilled(rc, S(3.f), fw::theme::U32(kPalette.accent), 12);
      }
      dl->AddText(fm, fm->FontSize, ImVec2(p0.x + S(32.f), p0.y + S(5.f)),
                  fw::theme::U32(sel ? kPalette.accent : kPalette.text),
                  o.label);
      dl->AddText(fs, fs->FontSize, ImVec2(p0.x + S(32.f), p0.y + S(22.f)),
                  fw::theme::U32(kPalette.text_dim), o.desc);
      if (clicked && out_mode != o.mode)
      {
        out_mode = o.mode;
        if (out_mode == OutMode::Card)
        {
          ShutdownAudio();
        }
        MarkSettingsDirty();
      }
      ImGui::Spacing();
    }
  }
  ImGui::EndChild();

  ImGui::EndChild(); // setup_right
  ImGui::PopStyleVar();
}

/* ── Overlays ────────────────────────────────────────────────────────── */

void App::DrawShortcutSheet()
{
  if (show_shortcuts && !ImGui::IsPopupOpen("##shortcuts"))
  {
    ImGui::OpenPopup("##shortcuts");
  }
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
             vp->WorkPos.y + vp->WorkSize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(S(420.f), 0.f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kPalette.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, kPalette.border_hi);
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.7f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(20.f, 16.f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S2(8.f, 10.f));
  bool open = show_shortcuts;
  if (ImGui::BeginPopupModal("##shortcuts", &open,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_AlwaysAutoResize))
  {
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float title_y = ImGui::GetCursorPosY();
    Mono("KEYBOARD SHORTCUTS", kPalette.accent, fw::theme::g_fonts.caps);
    ImGui::SetCursorPos(
        ImVec2(ImGui::GetCursorStartPos().x + content_w - S(28.f), title_y));
    if (fw::ui::ChipBtn("\u2715", false, BtnKind::Neutral))
    {
      show_shortcuts = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    struct Row
    {
      const char *keys;
      const char *desc;
    };
    static const Row kRows[] = {
        {"1\u20135", "Switch feature area"},
        {"Space", "Silence all voices"},
        {"A W S E D F T G Y H U J K", "Piano keys (C\u2013C)"},
        {"Z / X", "Octave down / up"},
        {"Cmd/Ctrl + \u2212 0", "UI zoom in / out / reset"},
        {"?", "This cheat-sheet"},
    };
    for (std::size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i)
    {
      const auto &r = kRows[i];
      Mono(r.keys, kPalette.accent_bright, fw::theme::g_fonts.mono);
      ImGui::SameLine();
      const float dw = ImGui::CalcTextSize(r.desc).x;
      ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + content_w - dw);
      ImGui::TextColored(kPalette.text_dim, "%s", r.desc);
      if (i + 1 < sizeof(kRows) / sizeof(kRows[0]))
      {
        ImGui::Separator();
      }
    }
    if (!show_shortcuts)
    {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (!open)
  {
    show_shortcuts = false;
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
}

void App::DrawAbout()
{
  if (!show_about)
  {
    return;
  }
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
             vp->WorkPos.y + vp->WorkSize.y * 0.5f),
      ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(S(420.f), 0.f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPalette.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, kPalette.border_hi);
  if (!ImGui::Begin("About", &show_about,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::End();
    ImGui::PopStyleColor(2);
    return;
  }
  ImGui::PushFont(fw::theme::g_fonts.hero);
  ImGui::TextColored(kPalette.accent, "%s", fw::product::kName);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.f);
  ImGui::TextColored(kPalette.text, "Control");
  ImGui::Spacing();
  ImGui::TextColored(kPalette.text_dim, "%s", fw::product::kTagline);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  Mono((std::string("Version   ") + fw::product::kVersion).c_str(),
       kPalette.text, fw::theme::g_fonts.mono);
  Mono((std::string("Build     ") + fw::product::kBuildType).c_str(),
       kPalette.text, fw::theme::g_fonts.mono);
  ImGui::Spacing();
  ImGui::TextWrapped(
      "RS485 console · MIDI input · USB CDC attack upload · USB BODY stream");
  ImGui::Spacing();
  ImGui::TextDisabled("Dear ImGui · GLFW · RtMidi · RtAudio");
  ImGui::Spacing();
  if (fw::ui::Btn("Keyboard shortcuts", ImVec2(-1.f, 0.f), BtnKind::Neutral))
  {
    show_shortcuts = true;
  }
  if (fw::ui::Btn("Close", ImVec2(-1.f, 0.f), BtnKind::Primary))
  {
    show_about = false;
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
}

/* ── Root layout ─────────────────────────────────────────────────────── */

void App::Draw()
{
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPalette.bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::Begin(fw::product::kTitle, nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar(); // ItemSpacing back to default inside children

  ImGuiIO &io = ImGui::GetIO();
  if (!io.WantTextInput)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_1))
    {
      view = GuiView::Perform;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2))
    {
      view = GuiView::Tone;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3))
    {
      view = GuiView::Sample;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_4))
    {
      view = GuiView::Effect;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_5))
    {
      view = GuiView::Setup;
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1))
    {
      show_about = !show_about;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Slash) && io.KeyShift)
    {
      show_shortcuts = !show_shortcuts;
    }
  }
  // UI zoom: Cmd/Ctrl + / − / 0. Fonts rebuild between frames (main loop).
  if (io.KeyCtrl || io.KeySuper)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
    {
      fw::theme::SetUserZoom(fw::theme::UserZoom() * 1.1f);
      ui_scale = fw::theme::UserZoom();
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
    {
      fw::theme::SetUserZoom(fw::theme::UserZoom() / 1.1f);
      ui_scale = fw::theme::UserZoom();
      MarkSettingsDirty();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_0))
    {
      fw::theme::SetUserZoom(1.f);
      ui_scale = 1.f;
      MarkSettingsDirty();
    }
  }

  DrawSidebar();
  ImGui::SameLine(0.f, 0.f);

  ImGui::BeginChild("main_col", ImVec2(0, 0), ImGuiChildFlags_None);
  DrawStatusBar();
  DrawBusFaultBanner();

  // Content row: page + right log panel
  const float log_w =
      S(log_collapsed ? Metrics::LogRailW : Metrics::LogW);
  ImGui::BeginChild("content", ImVec2(-log_w, 0), ImGuiChildFlags_None);
  if (view == GuiView::Perform)
  {
    DrawPerform();
  }
  else if (view == GuiView::Tone)
  {
    DrawTone();
  }
  else if (view == GuiView::Sample)
  {
    DrawSamplePage(*this);
  }
  else if (view == GuiView::Effect)
  {
    DrawEffectPanel(*this);
  }
  else if (view == GuiView::Setup)
  {
    DrawSetup();
  }
  ImGui::EndChild(); // content

  ImGui::SameLine(0.f, 0.f);
  DrawActivityLog();

  ImGui::EndChild(); // main_col

  DrawShortcutSheet();
  DrawAbout();
  toasts.Draw();
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}
