#include "env_editor.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

constexpr float kKMin = -10.f;
constexpr float kKMax = 10.f;

std::string FormatSlopeToken(float slope, float k)
{
  char buf[64];
  if (std::fabs(k) < 1e-5f) {
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(slope));
  } else if (k > 0.f) {
    std::snprintf(buf, sizeof(buf), "%.6g+%.6g", static_cast<double>(slope),
                  static_cast<double>(k));
  } else {
    // k already includes the minus sign in %g
    std::snprintf(buf, sizeof(buf), "%.6g%.6g", static_cast<double>(slope),
                  static_cast<double>(k));
  }
  return buf;
}

void DrawEnvelopeCurve(const EnvProgram &prog, ImVec2 size)
{
  float samples[256];
  float dur = 0.f;
  prog.SampleCurve(samples, 256, &dur);

  fw::ui::GlowWaveform("envcurve", samples, 256, size, 0.f, 1.f);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetItemRectMin();
  const ImVec2 p1 = ImGui::GetItemRectMax();
  const float w = p1.x - p0.x;
  const float h = p1.y - p0.y;

  // Segment boundary ticks
  float t_acc = 0.f;
  float start = 0.f;
  for (std::size_t i = 0; i < prog.segs.size(); ++i) {
    const bool rel = (i + 1 == prog.segs.size());
    const float d =
        EnvProgram::SegDuration(start, prog.segs[i], rel);
    t_acc += d;
    if (!rel) {
      start = prog.segs[i].end_amp;
    }
    if (dur > 1e-6f && i + 1 < prog.segs.size()) {
      const float x = p0.x + (t_acc / dur) * w;
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
                  IM_COL32(80, 120, 180, 90), 1.f);
    }
  }

  char label[64];
  std::snprintf(label, sizeof(label), "duration ~%.3f s (gate held at last end)",
                static_cast<double>(dur));
  ImGui::TextDisabled("%s", label);
  (void)h;
}

} // namespace

EnvProgram::EnvProgram() { ResetPluck(); }

void EnvProgram::EnsureValid()
{
  if (segs.size() < static_cast<std::size_t>(kMinSegs)) {
    ResetPluck();
    return;
  }
  if (segs.size() > static_cast<std::size_t>(kMaxSegs)) {
    segs.resize(static_cast<std::size_t>(kMaxSegs));
  }
  for (auto &s : segs) {
    s.end_amp = std::clamp(s.end_amp, 0.f, 1.f);
    if (s.slope < 0.001f) {
      s.slope = 0.001f;
    }
    s.k = std::clamp(s.k, kKMin, kKMax);
  }
  segs.back().end_amp = 0.f;
}

void EnvProgram::ResetPluck()
{
  segs = {
      {1.0f, 12.f, 0.f},
      {0.0f, 3.f, 0.f},
  };
}

void EnvProgram::ResetPad()
{
  segs = {
      {1.0f, 2.f, 0.5f},
      {0.65f, 0.8f, 0.25f},
      {0.0f, 0.4f, -0.5f},
  };
}

void EnvProgram::ResetOrgan()
{
  segs = {
      {1.0f, 40.f, 0.f},
      {0.0f, 40.f, 0.f},
  };
}

void EnvProgram::ResetSnappy()
{
  segs = {
      {1.0f, 80.f, 1.f},
      {0.25f, 20.f, 0.5f},
      {0.0f, 8.f, 0.f},
  };
}

bool EnvProgram::AddSegmentBeforeRelease()
{
  if (static_cast<int>(segs.size()) >= kMaxSegs) {
    return false;
  }
  const float mid =
      (segs.size() >= 2) ? (segs[segs.size() - 2].end_amp * 0.7f) : 0.7f;
  EnvSegment s;
  s.end_amp = std::clamp(mid, 0.f, 1.f);
  s.slope = 4.f;
  s.k = 0.f;
  segs.insert(segs.end() - 1, s);
  return true;
}

bool EnvProgram::RemoveSegmentBeforeRelease(int index)
{
  if (PreReleaseCount() <= 1) {
    return false;
  }
  if (index < 0 || index >= PreReleaseCount()) {
    return false;
  }
  segs.erase(segs.begin() + index);
  return true;
}

float EnvProgram::SegDuration(float start_amp, const EnvSegment &seg,
                              bool is_release)
{
  const float end = is_release ? 0.f : seg.end_amp;
  const float delta = std::fabs(end - start_amp);
  if (seg.slope < 1e-6f) {
    return 0.f;
  }
  return delta / seg.slope;
}

float EnvProgram::PitchRate(float freq_hz, float k)
{
  if (std::fabs(k) < 1e-6f || freq_hz <= 0.f) {
    return 1.f;
  }
  return std::pow(freq_hz / kC4Hz, k);
}

void EnvProgram::SampleCurve(float *out, int n, float *out_duration_sec) const
{
  if (!out || n <= 0) {
    return;
  }
  std::vector<float> times;
  std::vector<float> amps;
  times.push_back(0.f);
  amps.push_back(0.f);
  float t = 0.f;
  float amp = 0.f;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const bool rel = (i + 1 == segs.size());
    const float d = SegDuration(amp, segs[i], rel);
    t += d;
    amp = rel ? 0.f : segs[i].end_amp;
    times.push_back(t);
    amps.push_back(amp);
  }
  // Hold plateau marker: duplicate last pre-release level briefly in plot
  // by stretching — visual only; gate hold is infinite on card.
  const float total = (t > 1e-6f) ? t : 1.f;
  if (out_duration_sec) {
    *out_duration_sec = total;
  }
  for (int i = 0; i < n; ++i) {
    const float u = (n == 1) ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
    const float target_t = u * total;
    // piecewise linear
    float y = 0.f;
    for (std::size_t s = 1; s < times.size(); ++s) {
      if (target_t <= times[s] || s + 1 == times.size()) {
        const float t0 = times[s - 1];
        const float t1 = times[s];
        const float a0 = amps[s - 1];
        const float a1 = amps[s];
        const float span = std::max(t1 - t0, 1e-9f);
        const float f = (target_t - t0) / span;
        y = a0 + (a1 - a0) * std::clamp(f, 0.f, 1.f);
        break;
      }
    }
    out[i] = y;
  }
}

std::string EnvProgram::FormatTokens() const
{
  std::string out;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const bool rel = (i + 1 == segs.size());
    if (i) {
      out.push_back(' ');
    }
    if (!rel) {
      char end_buf[32];
      std::snprintf(end_buf, sizeof(end_buf), "%.6g",
                    static_cast<double>(segs[i].end_amp));
      out += end_buf;
      out.push_back(' ');
    }
    out += FormatSlopeToken(segs[i].slope, segs[i].k);
  }
  return out;
}

std::string EnvProgram::FormatCommand(int voice) const
{
  if (voice < 0) {
    return std::string("en ") + FormatTokens();
  }
  char prefix[8];
  std::snprintf(prefix, sizeof(prefix), "en%x ", voice & 15);
  return std::string(prefix) + FormatTokens();
}

void DrawOscillatorCard(App &app)
{
  ImGui::BeginChild("osc_card", ImVec2(0, 176), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("OSCILLATOR");
  ImGui::Spacing();

  const char *shapes[] = {"Sine", "Pulse", "Triangle"};
  fw::ui::SegmentedControl("##shape", shapes, 3, &app.shape_mode);
  ImGui::Spacing();

  if (app.shape_mode == 1) {
    ImGui::TextDisabled("Pulse duty");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##pulseduty", &app.shape_param, 0.1f, 0.9f, "%.3f");
  } else if (app.shape_mode == 2) {
    ImGui::TextDisabled("Triangle asymmetry (0.5 = symmetric)");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##triasym", &app.shape_param, 0.1f, 0.9f, "%.3f");
  } else {
    ImGui::TextDisabled("s — no shape parameter");
  }

  if (fw::ui::GlowButton("Apply shape", ImVec2(-1, 0))) {
    if (app.shape_mode == 0) {
      app.bus.QueueExec(rs485::Target::Channel, "s");
    } else if (app.shape_mode == 1) {
      char cmd[32];
      std::snprintf(cmd, sizeof(cmd), "p %.4g",
                    static_cast<double>(app.shape_param));
      app.bus.QueueExec(rs485::Target::Channel, cmd);
    } else {
      char cmd[32];
      std::snprintf(cmd, sizeof(cmd), "t %.4g",
                    static_cast<double>(app.shape_param));
      app.bus.QueueExec(rs485::Target::Channel, cmd);
    }
  }
  ImGui::EndChild();
}

void DrawFilterCard(App &app)
{
  ImGui::BeginChild("filter_card", ImVec2(0, 320), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("DIGITAL LPF (voices 0–7)");
  ImGui::Spacing();

  ImGui::TextDisabled("Voice");
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderInt("##filtvoice", &app.selected_voice, 0, 15, "n%x");
  const bool filt_ok = (app.selected_voice >= 0 && app.selected_voice <= 7);
  if (!filt_ok) {
    ImGui::TextColored(fw::theme::kPalette.warning,
                       "Filter applies to n0–n7 only (selected is n%x)",
                       app.selected_voice);
  }
  ImGui::Checkbox("Bypass (f=20000)", &app.filter_bypass);
  if (!app.filter_bypass) {
    ImGui::TextDisabled("Cutoff Hz");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##cutoff", &app.filter_hz_f, 20.f, 20000.f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("q (DF4 g)");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##q", &app.filter_q_f, 0.5f, 10.f, "%.2f");
  }
  ImGui::TextDisabled("Filter pitch-track fk");
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderFloat("##fk", &app.filter_k_f, 0.f, 10.f, "%.2f");
  ImGui::Spacing();

  ImGui::BeginDisabled(!filt_ok || !app.bus.IsOpen());
  char label1[32];
  std::snprintf(label1, sizeof(label1), "Apply to n%x",
               app.selected_voice & 7);
  if (fw::ui::GlowButton(label1, ImVec2(-1, 0))) {
    const int v = app.selected_voice & 7;
    char cmd[64];
    if (app.filter_bypass) {
      std::snprintf(cmd, sizeof(cmd), "f%x 20000", v);
    } else {
      std::snprintf(cmd, sizeof(cmd), "f%x %.4g %.4g", v,
                    static_cast<double>(app.filter_hz_f),
                    static_cast<double>(app.filter_q_f));
    }
    app.bus.QueueExec(rs485::Target::Channel, cmd);
    std::snprintf(cmd, sizeof(cmd), "fk%x %.4g", v,
                  static_cast<double>(app.filter_k_f));
    app.bus.QueueExec(rs485::Target::Channel, cmd);
  }
  if (fw::ui::GlowButton("Apply to all n0–n7", ImVec2(-1, 0))) {
    char cmd[64];
    if (app.filter_bypass) {
      std::snprintf(cmd, sizeof(cmd), "f 20000");
    } else {
      std::snprintf(cmd, sizeof(cmd), "f %.4g %.4g",
                    static_cast<double>(app.filter_hz_f),
                    static_cast<double>(app.filter_q_f));
    }
    app.bus.QueueExec(rs485::Target::Channel, cmd);
    std::snprintf(cmd, sizeof(cmd), "fk %.4g",
                  static_cast<double>(app.filter_k_f));
    app.bus.QueueExec(rs485::Target::Channel, cmd);
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("fc = fbase × (note_Hz / C4)^fk");
  ImGui::EndChild();
}

void DrawEnvelopeEditor(App &app)
{
  EnvProgram &prog = app.env;
  prog.EnsureValid();

  fw::ui::SectionHeader("ENVELOPE");
  ImGui::TextDisabled(
      "Linear segments -> hold at last end -> release to 0. Per-segment k: "
      "rate × (f/C4)^k");

  ImGui::SetNextItemWidth(80);
  ImGui::SliderInt("Edit voice", &app.selected_voice, 0, 15, "n%x");
  ImGui::SameLine();
  ImGui::Checkbox("Apply to all 16", &app.env_apply_all);

  if (fw::ui::GlowButton("Pluck")) {
    prog.ResetPluck();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Pad")) {
    prog.ResetPad();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Organ")) {
    prog.ResetOrgan();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Snappy")) {
    prog.ResetSnappy();
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("+ segment") && prog.AddSegmentBeforeRelease()) {
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%d / %d segs (incl. release)",
                      static_cast<int>(prog.segs.size()), EnvProgram::kMaxSegs);

  DrawEnvelopeCurve(prog, ImVec2(-1, 140));

  ImGui::BeginChild("segtable", ImVec2(0, 220), ImGuiChildFlags_Borders);
  if (ImGui::BeginTable("segs", 6,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Seg", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("End amp", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Slope /s", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Pitch k", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("~ms", ImGuiTableColumnFlags_WidthFixed, 56.f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 36.f);
    ImGui::TableHeadersRow();

    float start_amp = 0.f;
    for (int i = 0; i < static_cast<int>(prog.segs.size()); ++i) {
      ImGui::PushID(i);
      const bool is_rel = (i + 1 == static_cast<int>(prog.segs.size()));
      EnvSegment &seg = prog.segs[static_cast<std::size_t>(i)];

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (is_rel) {
        ImGui::TextColored(fw::theme::kPalette.warning, "Release");
      } else {
        ImGui::Text("Seg %d", i);
      }

      ImGui::TableNextColumn();
      if (is_rel) {
        ImGui::TextDisabled("-> 0 (fixed)");
      } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##end", &seg.end_amp, 0.f, 1.f, "%.3f");
      }

      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##slope", &seg.slope, 0.05f, 100.f, "%.3f",
                         ImGuiSliderFlags_Logarithmic);

      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##k", &seg.k, kKMin, kKMax, "%+.2f");

      ImGui::TableNextColumn();
      const float d = EnvProgram::SegDuration(start_amp, seg, is_rel);
      ImGui::Text("%.0f", static_cast<double>(d * 1000.f));

      ImGui::TableNextColumn();
      if (!is_rel && prog.PreReleaseCount() > 1) {
        if (ImGui::SmallButton("x")) {
          prog.RemoveSegmentBeforeRelease(i);
          ImGui::PopID();
          break;
        }
      }

      if (!is_rel) {
        start_amp = seg.end_amp;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();

  // Pitch-track preview for selected segment row focus — show rates at C3/C4/C5
  ImGui::TextUnformatted("Pitch-track preview (selected segment k)");
  static int k_preview_seg = 0;
  ImGui::SetNextItemWidth(120);
  ImGui::SliderInt("Segment##kprev", &k_preview_seg, 0,
                   std::max(0, static_cast<int>(prog.segs.size()) - 1));
  k_preview_seg =
      std::clamp(k_preview_seg, 0, static_cast<int>(prog.segs.size()) - 1);
  const float pk = prog.segs[static_cast<std::size_t>(k_preview_seg)].k;
  const float r_c3 = EnvProgram::PitchRate(130.812782f, pk);
  const float r_c4 = EnvProgram::PitchRate(EnvProgram::kC4Hz, pk);
  const float r_c5 = EnvProgram::PitchRate(523.251131f, pk);
  ImGui::Text("k=%+.2f -> rate @ C3 %.3f · C4 %.3f · C5 %.3f",
              static_cast<double>(pk), static_cast<double>(r_c3),
              static_cast<double>(r_c4), static_cast<double>(r_c5));
  ImGui::TextDisabled("k>0: higher notes faster; k<0: higher notes slower");

  ImGui::Separator();
  const std::string cmd = prog.FormatCommand(
      app.env_apply_all ? -1 : (app.selected_voice & 15));
  ImGui::TextWrapped("%s", cmd.c_str());
  ImGui::BeginDisabled(!app.bus.IsOpen());
  if (fw::ui::GlowButton("Apply envelope to card", ImVec2(-1, 36))) {
    app.bus.QueueExec(rs485::Target::Channel, cmd);
    app.log.Push(std::string("-> ") + cmd);
  }
  ImGui::EndDisabled();
  if (ImGui::Button("Query enN")) {
    char q[16];
    std::snprintf(q, sizeof(q), "en%x", app.selected_voice & 15);
    app.bus.QueueExec(rs485::Target::Channel, q);
  }
  ImGui::SameLine();
  if (ImGui::Button("Bulk ekN from preview k")) {
    char q[32];
    std::snprintf(q, sizeof(q), "ek%x %.4g", app.selected_voice & 15,
                  static_cast<double>(pk));
    app.bus.QueueExec(rs485::Target::Channel, q);
  }
  ImGui::TextDisabled(
      "Prefer per-segment slope±k on Apply; ek overrides all ks on that voice");
}

void DrawEffectPanel(App &app)
{
  ImGui::SeparatorText("Effect card");
  if (!app.bus.IsOpen()) {
    ImGui::TextColored(fw::theme::kPalette.warning,
                       "Connect RS485 bus — Effect shares the multi-drop line.");
  }

  ImGui::BeginChild("eff_power", ImVec2(ImGui::GetContentRegionAvail().x * 0.48f,
                                        168),
                    ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("POWER / AUDIO");
  if (fw::ui::GlowButton("Refresh status  e:s", ImVec2(-1, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "s");
  }
  if (fw::ui::GlowButton("48V phantom ON", ImVec2(-1, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "v 1");
  }
  if (fw::ui::GlowButton("48V phantom OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueExec(rs485::Target::Effect, "v 0");
  }
  if (fw::ui::GlowButton("AUDIO_EN ON", ImVec2(-1, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "a 1");
  }
  if (fw::ui::GlowButton("AUDIO_EN OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueExec(rs485::Target::Effect, "a 0");
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("eff_leds", ImVec2(0, 168), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("LEDS");
  if (fw::ui::GlowButton("Auto flash ON", ImVec2(-1, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "l 1");
  }
  if (fw::ui::GlowButton("Auto flash OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueExec(rs485::Target::Effect, "l 0");
  }
  if (fw::ui::GlowButton("Red ON", ImVec2((ImGui::GetContentRegionAvail().x - 8.f) * 0.5f, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "lr 1");
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Red OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueExec(rs485::Target::Effect, "lr 0");
  }
  if (fw::ui::GlowButton("Yellow ON", ImVec2((ImGui::GetContentRegionAvail().x - 8.f) * 0.5f, 0))) {
    app.bus.QueueExec(rs485::Target::Effect, "ly 1");
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Yellow OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueExec(rs485::Target::Effect, "ly 0");
  }
  ImGui::EndChild();

  ImGui::SeparatorText("USB ADC channel (Effect mic path)");
  ImGui::TextDisabled("Select which of 8 ADC channels streams over UAC");
  for (int ch = 1; ch <= 8; ++ch) {
    if (ch > 1) {
      ImGui::SameLine();
    }
    char label[8];
    std::snprintf(label, sizeof(label), " %d ", ch);
    if (ImGui::Button(label, ImVec2(40, 32))) {
      char cmd[16];
      std::snprintf(cmd, sizeof(cmd), "u %d", ch);
      app.bus.QueueExec(rs485::Target::Effect, cmd);
    }
  }
  if (ImGui::Button("Query u")) {
    app.bus.QueueExec(rs485::Target::Effect, "u");
  }

  ImGui::SeparatorText("Echo / lab shortcuts");
  ImGui::TextDisabled("Keep echo OFF while using MIDI / Control GUI");
  if (ImGui::Button("ec 0 (echo off)")) {
    app.bus.QueueExec(rs485::Target::Effect, "ec 0");
  }
  ImGui::SameLine();
  if (ImGui::Button("ec 1 (echo on)")) {
    app.bus.QueueExec(rs485::Target::Effect, "ec 1");
  }
  if (ImGui::Button("I2C scan")) {
    app.bus.QueueExec(rs485::Target::Effect, "i2c");
  }
  ImGui::SameLine();
  if (ImGui::Button("ADC init  ai")) {
    app.bus.QueueExec(rs485::Target::Effect, "ai");
  }
}
