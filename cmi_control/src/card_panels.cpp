#include "card_panels.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "cardproto/channel.hpp"
#include "cardproto/effect.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using fw::theme::kPalette;
using fw::theme::S;
using fw::theme::S2;
using fw::ui::BtnKind;

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

/** Design row: mono label (min width 80) + inline content; bottom hairline.
 * Caller draws content after this and then calls RowEnd(). */
void RowBegin(const char *label)
{
  MonoText(label, kPalette.text_dim, fw::theme::g_fonts.caps);
  ImGui::SameLine(S(92.f));
}

void RowEnd()
{
  ImGui::Spacing();
  ImGui::Separator();
}

void FormatHz(float hz, char *buf, std::size_t n)
{
  if (hz < 1000.f) {
    std::snprintf(buf, n, "%.0f Hz", static_cast<double>(hz));
  } else {
    std::snprintf(buf, n, "%.2f kHz", static_cast<double>(hz / 1000.f));
  }
}

} // namespace

/* ── OSCILLATOR panel (design Tone left column) ──────────────────────── */

void DrawOscillatorCard(App &app)
{
  const bool offline = !app.bus.IsOpen() || app.bus.BusFault();
  ImFont *fs = fw::theme::g_fonts.mono_small;

  const bool has_param = (app.shape_mode != 0);
  fw::ui::BeginSection("osc_card", "OSCILLATOR",
                       ImVec2(0, S(has_param ? 216.f : 188.f)));
  ImGui::NewLine();
  ImGui::Spacing();

  // Shape row
  RowBegin("Shape");
  {
    struct ShapeOpt
    {
      const char *label;
      int mode;
    };
    static const ShapeOpt kShapes[] = {
        {"Sine", 0}, {"Pulse", 1}, {"Tri", 2}};
    for (const auto &s : kShapes) {
      if (s.mode) {
        ImGui::SameLine(0.f, 4.f);
      }
      if (fw::ui::ChipBtn(s.label, app.shape_mode == s.mode)) {
        app.shape_mode = s.mode;
      }
    }
  }
  RowEnd();

  if (app.shape_mode == 1) {
    RowBegin("Duty");
    ImGui::SetNextItemWidth(S(100.f));
    ImGui::SliderFloat("##duty", &app.shape_param, 0.1f, 0.9f, "");
    ImGui::SameLine(0.f, S(8.f));
    char v[16];
    std::snprintf(v, sizeof(v), "%.2f", static_cast<double>(app.shape_param));
    MonoText(v, kPalette.accent, fw::theme::g_fonts.mono);
    RowEnd();
  } else if (app.shape_mode == 2) {
    RowBegin("Asym");
    ImGui::SetNextItemWidth(S(100.f));
    ImGui::SliderFloat("##asym", &app.shape_param, 0.1f, 0.9f, "");
    ImGui::SameLine(0.f, S(8.f));
    char v[16];
    std::snprintf(v, sizeof(v), "%.2f", static_cast<double>(app.shape_param));
    MonoText(v, kPalette.accent, fw::theme::g_fonts.mono);
    RowEnd();
  }

  ImGui::Spacing();

  // Wave preview (120×40) + APPLY, bottom row
  {
    const float pw = S(120.f);
    const float ph = S(40.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph),
                      fw::theme::U32(kPalette.bg_alt), S(2.f));
    dl->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph),
                fw::theme::U32(kPalette.border), S(2.f));
    dl->AddLine(ImVec2(p0.x, p0.y + ph * 0.5f),
                ImVec2(p0.x + pw, p0.y + ph * 0.5f),
                fw::theme::U32A(kPalette.accent, 0.10f), 1.f);
    constexpr int kN = 120;
    ImVec2 pts[kN + 1];
    for (int i = 0; i <= kN; ++i) {
      const float t = static_cast<float>(i) / kN;
      float y;
      if (app.shape_mode == 0) {
        y = std::sin(2.f * 3.14159265f * t);
      } else if (app.shape_mode == 1) {
        y = t < app.shape_param ? 1.f : -1.f;
      } else {
        const float a = std::clamp(app.shape_param, 0.05f, 0.95f);
        y = t < a ? (2.f * t / a) - 1.f : 1.f - 2.f * (t - a) / (1.f - a);
      }
      pts[i] = ImVec2(p0.x + t * pw, p0.y + ph * 0.5f - y * ph * 0.38f);
    }
    dl->AddPolyline(pts, kN + 1, fw::theme::U32A(kPalette.accent, 0.35f), 0,
                    3.f);
    dl->AddPolyline(pts, kN + 1, fw::theme::U32(kPalette.accent), 0, 1.5f);
    ImGui::Dummy(ImVec2(pw, ph));

    ImGui::SameLine(ImGui::GetWindowWidth() - S(80.f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ph - S(26.f));
    ImGui::BeginDisabled(offline);
    if (fw::ui::Btn("APPLY", S2(64.f, 24.f), BtnKind::Primary)) {
      const float p = app.shape_param;
      BusQueueResult r = BusQueueResult::Closed;
      if (app.shape_mode == 0) {
        r = app.bus.QueueChannel(
            [](cardproto::ChannelClient &ch) { return ch.Sine(); });
      } else if (app.shape_mode == 1) {
        r = app.bus.QueueChannel([p](cardproto::ChannelClient &ch) {
          return ch.Pulse(static_cast<double>(p));
        });
      } else {
        r = app.bus.QueueChannel([p](cardproto::ChannelClient &ch) {
          return ch.Triangle(static_cast<double>(p));
        });
      }
      app.NotifyEnqueue(r == BusQueueResult::Ok, "bus offline");
    }
    ImGui::EndDisabled();
  }
  if (offline) {
    MonoText("Bus offline — connect in Setup to apply", kPalette.muted, fs);
  }
  fw::ui::EndSection();
}

/* ── FILTER panel (design: n0–n7 LPF) ────────────────────────────────── */

void DrawFilterCard(App &app)
{
  const bool offline = !app.bus.IsOpen() || app.bus.BusFault();
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;

  fw::ui::BeginSection("filter_card", "FILTER (n0\u2013n7 only)",
                       ImVec2(0, S(248.f)));
  ImGui::NewLine();
  ImGui::Spacing();

  MonoText("Voices n8\u2013nf have no filter path", kPalette.muted, fs);
  ImGui::Spacing();

  RowBegin("Bypass");
  {
    bool bypass = app.filter_bypass;
    if (ImGui::Checkbox("##bypass", &bypass)) {
      app.filter_bypass = bypass;
    }
    ImGui::SameLine(0.f, S(8.f));
    MonoText("Wide-open (bypass)", kPalette.text_dim, fs);
  }
  RowEnd();

  ImGui::BeginDisabled(app.filter_bypass);
  RowBegin("Cutoff");
  ImGui::SetNextItemWidth(S(110.f));
  ImGui::SliderFloat("##cutoff", &app.filter_hz_f, 20.f, 20000.f, "",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine(0.f, S(8.f));
  {
    char hz[24];
    FormatHz(app.filter_hz_f, hz, sizeof(hz));
    MonoText(hz, app.filter_bypass ? kPalette.muted : kPalette.accent, fm);
  }
  RowEnd();

  RowBegin("Q");
  ImGui::SetNextItemWidth(S(110.f));
  ImGui::SliderFloat("##q", &app.filter_q_f, 0.5f, 10.f, "");
  ImGui::SameLine(0.f, S(8.f));
  {
    char q[16];
    std::snprintf(q, sizeof(q), "%.1f", static_cast<double>(app.filter_q_f));
    MonoText(q, app.filter_bypass ? kPalette.muted : kPalette.accent, fm);
  }
  RowEnd();

  RowBegin("Pitch (fk)");
  ImGui::SetNextItemWidth(S(110.f));
  ImGui::SliderFloat("##fk", &app.filter_k_f, 0.f, 10.f, "");
  app.filter_k_f = std::round(app.filter_k_f * 2.f) * 0.5f;
  ImGui::SameLine(0.f, S(8.f));
  {
    char k[16];
    std::snprintf(k, sizeof(k), "%.1f", static_cast<double>(app.filter_k_f));
    MonoText(k, app.filter_bypass ? kPalette.muted : kPalette.accent, fm);
  }
  RowEnd();
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::BeginDisabled(offline);
  /* One apply: the shared LPF path covers n0–n7 as a group on hardware. */
  if (fw::ui::Btn("APPLY n0\u2013n7", ImVec2(-1, S(24.f)),
                  BtnKind::Primary)) {
    const bool bypass = app.filter_bypass;
    const double hz = static_cast<double>(app.filter_hz_f);
    const double q = static_cast<double>(app.filter_q_f);
    const double fk = static_cast<double>(app.filter_k_f);
    app.log.Push(LogKind::Tx,
                 bypass ? std::string("tx filter n0..n7 bypass")
                        : "tx filter n0..n7 fc=" +
                              std::to_string(static_cast<int>(hz)) +
                              " q=" + std::to_string(q) +
                              " fk=" + std::to_string(fk));
    app.bus.QueueChannel([bypass, hz, q](cardproto::ChannelClient &ch) {
      return bypass ? ch.SetFilters(20000.0) : ch.SetFilters(hz, q);
    });
    app.bus.QueueChannel(
        [fk](cardproto::ChannelClient &ch) { return ch.SetFk(fk); });
  }
  ImGui::EndDisabled();
  if (offline) {
    MonoText("Bus offline", kPalette.muted, fs);
  }
  fw::ui::EndSection();
}

/* ── EFFECT page ─────────────────────────────────────────────────────── */

namespace
{

void LedDot(const ImVec4 &col, bool on)
{
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float line_h = ImGui::GetTextLineHeight();
  const float r = S(6.f);
  const ImVec2 c(p.x + r, p.y + line_h * 0.5f);
  if (on) {
    dl->AddCircleFilled(c, r * 1.5f, fw::theme::U32A(col, 0.25f), 16);
    dl->AddCircleFilled(c, r, fw::theme::U32(col), 16);
  } else {
    dl->AddCircleFilled(c, r, fw::theme::U32(kPalette.bg_alt), 16);
    dl->AddCircle(c, r, fw::theme::U32(kPalette.border_hi), 16, 1.f);
  }
  ImGui::Dummy(ImVec2(r * 2.f, line_h));
}

} // namespace

void DrawEffectPanel(App &app)
{
  ImFont *fs = fw::theme::g_fonts.mono_small;
  const bool bus_online = app.bus.IsOpen() && !app.bus.BusFault();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(12.f, 12.f));
  ImGui::BeginChild("effect_page", ImVec2(0, 0), ImGuiChildFlags_None);
  ImGui::PopStyleVar();

  // Honesty note
  {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(kPalette.warning.x, kPalette.warning.y,
                                 kPalette.warning.z, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImVec4(kPalette.warning.x, kPalette.warning.y,
                                 kPalette.warning.z, 0.15f));
    ImGui::BeginChild("eff_note", ImVec2(0, S(46.f)), ImGuiChildFlags_Borders);
    ImGui::SetCursorPos(S2(10.f, 8.f));
    fw::ui::WarnIcon(kPalette.warning);
    ImGui::SameLine(0.f, S(8.f));
    ImGui::PushFont(fs);
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - S(10.f));
    ImGui::TextColored(
        kPalette.text_dim,
        "Toggles reflect last command sent, not confirmed hardware state. "
        "Use Query Status to refresh from the card.%s",
        bus_online ? "" : " Bus offline — sends will fail until connected.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
  }
  ImGui::Spacing();

  const float gap = S(12.f);
  const float col_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;

  // ── POWER / AUDIO
  fw::ui::BeginSection("eff_power", "POWER / AUDIO",
                       ImVec2(col_w, S(app.effect.echo ? 244.f : 196.f)));
  ImGui::NewLine();
  ImGui::Spacing();
  if (fw::ui::ToggleRow("48V Phantom Power", &app.effect.phantom, true)) {
    const bool v = app.effect.phantom;
    app.log.Push(LogKind::Tx,
                 std::string("tx effect phantom ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.Set48V(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  if (fw::ui::ToggleRow("Audio Enable", &app.effect.audio_en, true)) {
    const bool v = app.effect.audio_en;
    app.log.Push(LogKind::Tx,
                 std::string("tx effect audio ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.SetAudioEn(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  if (fw::ui::ToggleRow("RS485 Echo", &app.effect.echo, true)) {
    const bool v = app.effect.echo;
    app.log.Push(LogKind::Tx,
                 std::string("tx effect echo ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.SetEcho(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  if (app.effect.echo) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(kPalette.danger.x, kPalette.danger.y,
                                 kPalette.danger.z, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImVec4(kPalette.danger.x, kPalette.danger.y,
                                 kPalette.danger.z, 0.20f));
    ImGui::BeginChild("echo_warn", ImVec2(0, S(42.f)),
                      ImGuiChildFlags_Borders);
    ImGui::SetCursorPos(S2(8.f, 5.f));
    ImGui::PushFont(fs);
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - S(8.f));
    ImGui::TextColored(kPalette.danger,
                       "\u26A0 RS485 echo is ON. This disrupts MIDI and "
                       "control messages. Disable after bring-up.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
  }
  ImGui::Spacing();
  if (fw::ui::Btn("\u21A9 Query Status", ImVec2(-1, S(24.f)),
                  BtnKind::Neutral)) {
    app.log.Push(LogKind::Tx, "tx effect query");
    app.NotifyEnqueue(app.bus.QueueEffect([](cardproto::EffectClient &e) {
                        return e.Status();
                      }) == BusQueueResult::Ok,
                      "bus offline");
  }
  fw::ui::EndSection();

  // ── STATUS LEDs
  ImGui::SameLine(0.f, gap);
  fw::ui::BeginSection("eff_leds", "STATUS LEDs",
                       ImVec2(0, S(app.effect.echo ? 244.f : 196.f)));
  ImGui::NewLine();
  ImGui::Spacing();
  {
    MonoText("Live preview:", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, S(12.f));
    LedDot(kPalette.accent, app.effect.led_flash);
    ImGui::SameLine(0.f, S(10.f));
    LedDot(kPalette.danger, app.effect.led_red);
    ImGui::SameLine(0.f, S(10.f));
    LedDot(kPalette.warning, app.effect.led_yellow);
    ImGui::Spacing();
    ImGui::Separator();
  }
  if (fw::ui::ToggleRow("Auto Flash", &app.effect.led_flash, true)) {
    const bool v = app.effect.led_flash;
    app.log.Push(LogKind::Tx,
                 std::string("tx led autoflash ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.SetLedFlash(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  if (fw::ui::ToggleRow("Red LED", &app.effect.led_red, true)) {
    const bool v = app.effect.led_red;
    app.log.Push(LogKind::Tx,
                 std::string("tx led red ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.SetLedRed(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  if (fw::ui::ToggleRow("Yellow LED", &app.effect.led_yellow, true)) {
    const bool v = app.effect.led_yellow;
    app.log.Push(LogKind::Tx,
                 std::string("tx led yellow ") + (v ? "on" : "off"));
    app.NotifyEnqueue(
        app.bus.QueueEffect(
            [v](cardproto::EffectClient &e) { return e.SetLedYellow(v); }) ==
            BusQueueResult::Ok,
        "bus offline");
  }
  fw::ui::EndSection();

  ImGui::Spacing();

  // ── USB ADC CHANNEL
  fw::ui::BeginSection("eff_usb", "USB ADC CHANNEL", ImVec2(col_w, S(152.f)));
  ImGui::NewLine();
  ImGui::Spacing();
  MonoText("Select which of the 8 ADC input channels streams over USB audio.",
           kPalette.text_dim, fs);
  ImGui::Spacing();
  for (int ch = 1; ch <= 8; ++ch) {
    if (ch > 1) {
      ImGui::SameLine(0.f, S(5.f));
    }
    char label[8];
    std::snprintf(label, sizeof(label), "%d", ch);
    if (fw::ui::ChipBtn(label, app.effect.usb_adc_ch == ch)) {
      app.effect.usb_adc_ch = ch;
      const uint8_t uch = static_cast<uint8_t>(ch);
      app.log.Push(LogKind::Tx,
                   std::string("tx adc channel ") + std::to_string(ch));
      app.NotifyEnqueue(
          app.bus.QueueEffect([uch](cardproto::EffectClient &e) {
            return e.SetUsbAdcCh(uch);
          }) == BusQueueResult::Ok,
          "bus offline");
    }
  }
  ImGui::Spacing();
  if (fw::ui::Btn("\u21A9 Query Current Channel", ImVec2(0, S(24.f)),
                  BtnKind::Neutral)) {
    app.log.Push(LogKind::Tx, "tx adc channel query");
    app.NotifyEnqueue(app.bus.QueueEffect([](cardproto::EffectClient &e) {
                        return e.GetUsbAdcCh();
                      }) == BusQueueResult::Ok,
                      "bus offline");
  }
  fw::ui::EndSection();

  // ── ADC / I2C TOOLS
  ImGui::SameLine(0.f, gap);
  fw::ui::BeginSection("eff_adc", "ADC / I2C TOOLS", ImVec2(0, S(152.f)));
  ImGui::NewLine();
  ImGui::Spacing();
  {
    if (fw::ui::ChipBtn("READ", app.effect.adc_mode == 0)) {
      app.effect.adc_mode = 0;
    }
    ImGui::SameLine(0.f, S(4.f));
    if (fw::ui::ChipBtn("WRITE", app.effect.adc_mode == 1)) {
      app.effect.adc_mode = 1;
    }
  }
  {
    MonoText("Chip", kPalette.text_dim, fs);
    ImGui::SameLine(S(48.f));
    ImGui::SetNextItemWidth(S(64.f));
    int chip = app.effect.adc_chip;
    ImGui::InputInt("##chip", &chip);
    app.effect.adc_chip = static_cast<uint8_t>(std::clamp(chip, 0, 255));

    MonoText("Reg", kPalette.text_dim, fs);
    ImGui::SameLine(S(48.f));
    ImGui::SetNextItemWidth(S(64.f));
    int reg = app.effect.adc_reg;
    ImGui::InputInt("##reg", &reg);
    app.effect.adc_reg = static_cast<uint8_t>(std::clamp(reg, 0, 255));

    if (app.effect.adc_mode == 1) {
      MonoText("Val", kPalette.text_dim, fs);
      ImGui::SameLine(S(48.f));
      ImGui::SetNextItemWidth(S(64.f));
      int val = app.effect.adc_val;
      ImGui::InputInt("##val", &val);
      app.effect.adc_val = static_cast<uint8_t>(std::clamp(val, 0, 255));
    }
  }
  ImGui::Spacing();
  {
    const bool write_mode = (app.effect.adc_mode == 1);
    if (fw::ui::Btn(write_mode ? "Write" : "Read", S2(64.f, 24.f),
                    BtnKind::Primary)) {
      const uint8_t c = app.effect.adc_chip;
      const uint8_t r = app.effect.adc_reg;
      const uint8_t v = app.effect.adc_val;
      if (write_mode) {
        app.log.Push(LogKind::Tx, "tx adc write chip=" + std::to_string(c) +
                                      " reg=" + std::to_string(r) +
                                      " val=" + std::to_string(v));
        app.NotifyEnqueue(
            app.bus.QueueEffect([c, r, v](cardproto::EffectClient &e) {
              return e.AdcWrite(c, r, v);
            }) == BusQueueResult::Ok,
            "bus offline");
      } else {
        app.log.Push(LogKind::Tx, "tx adc read chip=" + std::to_string(c) +
                                      " reg=" + std::to_string(r));
        app.NotifyEnqueue(
            app.bus.QueueEffect([c, r](cardproto::EffectClient &e) {
              return e.AdcRead(c, r);
            }) == BusQueueResult::Ok,
            "bus offline");
      }
    }
    ImGui::SameLine(0.f, S(6.f));
    if (fw::ui::Btn("I2C Scan", ImVec2(0, S(24.f)), BtnKind::Neutral)) {
      app.log.Push(LogKind::Tx, "tx i2c scan");
      app.NotifyEnqueue(app.bus.QueueEffect([](cardproto::EffectClient &e) {
                          return e.I2cScan();
                        }) == BusQueueResult::Ok,
                        "bus offline");
    }
    ImGui::SameLine(0.f, S(6.f));
    if (fw::ui::Btn("ADC Init", ImVec2(0, S(24.f)), BtnKind::Neutral)) {
      app.log.Push(LogKind::Tx, "tx adc init");
      app.NotifyEnqueue(app.bus.QueueEffect([](cardproto::EffectClient &e) {
                          return e.AdcInit();
                        }) == BusQueueResult::Ok,
                        "bus offline");
    }
  }
  fw::ui::EndSection();

  ImGui::EndChild();
}

