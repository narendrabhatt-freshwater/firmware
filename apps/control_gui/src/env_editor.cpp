#include "env_editor.hpp"

#include "app.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "protocol/channel.hpp"
#include "protocol/effect.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

/**
 * Interactive envelope curve: click empty space to split a segment and add
 * a breakpoint there, drag an existing breakpoint to reshape (vertical =
 * amplitude, horizontal = re-solves that segment's slope so the point lands
 * at the dragged time). Breakpoint 0 (start, fixed at t=0/amp=0) isn't
 * draggable; the release point's amplitude is pinned to 0 but its time
 * (via slope) is.
 */
void DrawEnvelopeCurveEditor(EnvProgram &prog, ImVec2 size_arg)
{
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 size(size_arg.x <= 0.f ? avail.x + size_arg.x : size_arg.x,
                    size_arg.y <= 0.f ? avail.y + size_arg.y : size_arg.y);

  float samples[256];
  float dur = 0.f;
  prog.SampleCurve(samples, 256, &dur);

  const int n = static_cast<int>(prog.segs.size());
  std::vector<float> times(static_cast<std::size_t>(n) + 1);
  std::vector<float> amps(static_cast<std::size_t>(n) + 1);
  times[0] = 0.f;
  amps[0] = 0.f;
  for (int i = 0; i < n; ++i) {
    const bool rel = (i + 1 == n);
    const float d = EnvProgram::SegDuration(amps[static_cast<std::size_t>(i)],
                                            prog.segs[static_cast<std::size_t>(i)], rel);
    times[static_cast<std::size_t>(i + 1)] = times[static_cast<std::size_t>(i)] + d;
    amps[static_cast<std::size_t>(i + 1)] =
        rel ? 0.f : prog.segs[static_cast<std::size_t>(i)].end_amp;
  }
  const float total = std::max(times.back(), 1e-6f);

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  const float w = size.x;
  const float h = size.y;

  auto ScreenOf = [&](int i) {
    const float u = times[static_cast<std::size_t>(i)] / total;
    const float v = amps[static_cast<std::size_t>(i)];
    return ImVec2(p0.x + u * w, p0.y + h - v * h);
  };

  dl->AddRectFilled(p0, p1, fw::theme::U32A(fw::theme::kPalette.panel_alt, 0.55f), 8.f);
  dl->AddRect(p0, p1, fw::theme::U32A(fw::theme::kPalette.border, 0.7f), 8.f);

  ImGui::InvisibleButton("envcurve", size);
  const bool canvas_hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // Segment boundary ticks
  for (int i = 1; i < n; ++i) {
    const float x = p0.x + (times[static_cast<std::size_t>(i)] / total) * w;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(80, 120, 180, 90), 1.f);
  }

  // Glow curve
  dl->PushClipRect(p0, p1, true);
  {
    ImVec2 pts[256];
    for (int i = 0; i < 256; ++i) {
      const float u = static_cast<float>(i) / 255.f;
      pts[i] = ImVec2(p0.x + u * w, p0.y + h - samples[i] * h);
    }
    dl->AddPolyline(pts, 256, fw::theme::U32A(fw::theme::kPalette.accent, 0.10f), 0, 6.f);
    dl->AddPolyline(pts, 256, fw::theme::U32A(fw::theme::kPalette.accent, 0.24f), 0, 3.5f);
    dl->AddPolyline(pts, 256, fw::theme::U32(fw::theme::kPalette.accent), 0, 1.6f);
  }
  dl->PopClipRect();

  static int s_drag_idx = -1; // 1..n -> segs[idx-1]; -1 = none

  // Hit-test breakpoints 1..n (0 is the fixed start, not draggable).
  int hovered_handle = -1;
  if (canvas_hovered || s_drag_idx >= 0) {
    constexpr float kPickR = 11.f;
    for (int i = 1; i <= n; ++i) {
      const ImVec2 hp = ScreenOf(i);
      const float dx = mouse.x - hp.x;
      const float dy = mouse.y - hp.y;
      if (dx * dx + dy * dy <= kPickR * kPickR) {
        hovered_handle = i;
        break;
      }
    }
  }

  // Ghost "+" affordance over empty canvas.
  if (canvas_hovered && hovered_handle < 0 && s_drag_idx < 0 &&
      n < EnvProgram::kMaxSegs) {
    dl->AddCircle(mouse, 6.f, fw::theme::U32A(fw::theme::kPalette.text, 0.5f), 12, 1.5f);
    dl->AddLine(ImVec2(mouse.x - 3.f, mouse.y), ImVec2(mouse.x + 3.f, mouse.y),
               fw::theme::U32A(fw::theme::kPalette.text, 0.7f), 1.5f);
    dl->AddLine(ImVec2(mouse.x, mouse.y - 3.f), ImVec2(mouse.x, mouse.y + 3.f),
               fw::theme::U32A(fw::theme::kPalette.text, 0.7f), 1.5f);
  }

  // Draw breakpoints (skip 0: fixed start).
  for (int i = 1; i <= n; ++i) {
    const ImVec2 hp = ScreenOf(i);
    const bool active_h = (i == s_drag_idx) || (i == hovered_handle);
    const float r = active_h ? 7.f : 5.f;
    dl->AddCircleFilled(hp, r, fw::theme::U32(fw::theme::kPalette.accent), 14);
    dl->AddCircle(hp, r, fw::theme::U32(fw::theme::kPalette.bg), 14, 1.5f);
  }

  // Continue an active drag.
  if (s_drag_idx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    const int segi = s_drag_idx - 1;
    const bool is_release = (s_drag_idx == n);
    const float px_per_sec = (total > 1e-6f) ? w / total : 0.f;
    const float dt = (px_per_sec > 1e-6f) ? d.x / px_per_sec : 0.f;
    const float prev_time = times[static_cast<std::size_t>(s_drag_idx - 1)];
    const float new_time =
        std::max(times[static_cast<std::size_t>(s_drag_idx)] + dt, prev_time + 0.02f);
    const float prev_amp = amps[static_cast<std::size_t>(s_drag_idx - 1)];
    const float new_amp =
        is_release ? 0.f
                   : std::clamp(amps[static_cast<std::size_t>(s_drag_idx)] - d.y / h, 0.f, 1.f);
    const float new_slope = std::fabs(new_amp - prev_amp) / std::max(new_time - prev_time, 0.02f);
    prog.segs[static_cast<std::size_t>(segi)].end_amp = new_amp;
    prog.segs[static_cast<std::size_t>(segi)].slope = std::clamp(new_slope, 0.05f, 200.f);
  } else if (s_drag_idx >= 0) {
    s_drag_idx = -1; // mouse released
  }

  // Start a drag, or add a new breakpoint by splitting the clicked segment.
  if (canvas_hovered && s_drag_idx < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hovered_handle >= 0) {
      s_drag_idx = hovered_handle;
    } else if (n < EnvProgram::kMaxSegs) {
      const float click_time = std::clamp((mouse.x - p0.x) / w, 0.f, 1.f) * total;
      const float click_amp = std::clamp((p1.y - mouse.y) / h, 0.f, 1.f);

      int k = n - 1;
      for (int i = 0; i < n; ++i) {
        if (click_time <= times[static_cast<std::size_t>(i + 1)]) {
          k = i;
          break;
        }
      }
      const float prev_amp = (k == 0) ? 0.f : prog.segs[static_cast<std::size_t>(k - 1)].end_amp;
      const bool was_release = (k + 1 == n);
      const float orig_end = was_release ? 0.f : prog.segs[static_cast<std::size_t>(k)].end_amp;
      const float t_k = times[static_cast<std::size_t>(k)];
      const float t_k1 = times[static_cast<std::size_t>(k + 1)];

      EnvSegment seg_a;
      seg_a.end_amp = click_amp;
      seg_a.slope = std::clamp(
          std::fabs(click_amp - prev_amp) / std::max(click_time - t_k, 0.02f), 0.05f, 200.f);
      seg_a.k = prog.segs[static_cast<std::size_t>(k)].k;

      EnvSegment seg_b = prog.segs[static_cast<std::size_t>(k)];
      seg_b.end_amp = orig_end;
      seg_b.slope = std::clamp(
          std::fabs(orig_end - click_amp) / std::max(t_k1 - click_time, 0.02f), 0.05f, 200.f);

      prog.segs[static_cast<std::size_t>(k)] = seg_b;
      prog.segs.insert(prog.segs.begin() + k, seg_a);
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
  ImGui::TextDisabled("duration ~%.3f s (gate held at last end) · click to add a point, drag to reshape",
                      static_cast<double>(dur));
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
    const float p = app.shape_param;
    if (app.shape_mode == 0) {
      app.bus.QueueChannel([](protocol::ChannelClient &ch) {
        return ch.Sine();
      });
    } else if (app.shape_mode == 1) {
      app.bus.QueueChannel([p](protocol::ChannelClient &ch) {
        return ch.Pulse(static_cast<double>(p));
      });
    } else {
      app.bus.QueueChannel([p](protocol::ChannelClient &ch) {
        return ch.Triangle(static_cast<double>(p));
      });
    }
  }
  ImGui::EndChild();
}

void DrawFilterCard(App &app)
{
  ImGui::BeginChild("filter_card", ImVec2(0, 320), ImGuiChildFlags_Borders);
  fw::ui::SectionHeader("DIGITAL LPF (voices 0–7)");
  ImGui::Spacing();

  ImGui::TextDisabled("Voice (filter is n0–n7 only)");
  fw::ui::VoiceSelector("##filtvoice", &app.selected_voice, 8);
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
    const uint8_t v = static_cast<uint8_t>(app.selected_voice & 7);
    const bool bypass = app.filter_bypass;
    const double hz = static_cast<double>(app.filter_hz_f);
    const double q = static_cast<double>(app.filter_q_f);
    const double fk = static_cast<double>(app.filter_k_f);
    app.bus.QueueChannel([v, bypass, hz, q](protocol::ChannelClient &ch) {
      return bypass ? ch.SetFilter(v, 20000.0)
                    : ch.SetFilter(v, hz, q);
    });
    app.bus.QueueChannel([v, fk](protocol::ChannelClient &ch) {
      return ch.SetFk(v, fk);
    });
  }
  if (fw::ui::GlowButton("Apply to all n0–n7", ImVec2(-1, 0))) {
    const bool bypass = app.filter_bypass;
    const double hz = static_cast<double>(app.filter_hz_f);
    const double q = static_cast<double>(app.filter_q_f);
    const double fk = static_cast<double>(app.filter_k_f);
    app.bus.QueueChannel([bypass, hz, q](protocol::ChannelClient &ch) {
      return bypass ? ch.SetFilters(20000.0) : ch.SetFilters(hz, q);
    });
    app.bus.QueueChannel([fk](protocol::ChannelClient &ch) {
      return ch.SetFk(fk);
    });
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("fc = fbase × (note_Hz / C4)^fk");
  ImGui::EndChild();
}

void DrawVoiceOverview(App &app)
{
  fw::ui::SectionHeader("ALL VOICES");
  ImGui::TextDisabled("Click a voice to edit its envelope below");

  const float gap = 6.f;
  const float cell_w = (ImGui::GetContentRegionAvail().x - gap * 7.f) / 8.f;
  const float cell_h = 52.f;

  for (int i = 0; i < static_cast<int>(midi_host::kVoiceCount); ++i) {
    if (i % 8 != 0) {
      ImGui::SameLine(0.f, gap);
    }
    ImGui::PushID(i);
    const bool sel = (app.selected_voice == i);

    float samples[48];
    float dur = 0.f;
    app.voice_envs[static_cast<std::size_t>(i)].SampleCurve(samples, 48, &dur);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##thumb", ImVec2(cell_w, cell_h));
    if (ImGui::IsItemClicked()) {
      app.selected_voice = i;
    }
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(pos, ImVec2(pos.x + cell_w, pos.y + cell_h),
                      fw::theme::U32A(fw::theme::kPalette.panel_alt, sel ? 0.9f : 0.55f),
                      6.f);
    dl->AddRect(pos, ImVec2(pos.x + cell_w, pos.y + cell_h),
               sel ? fw::theme::U32(fw::theme::kPalette.accent)
                   : fw::theme::U32A(fw::theme::kPalette.border, hovered ? 0.9f : 0.6f),
               6.f, 0, sel ? 2.f : 1.f);

    ImVec2 pts[48];
    const float pad = 4.f;
    const float track_h = cell_h - 18.f;
    for (int s = 0; s < 48; ++s) {
      const float u = static_cast<float>(s) / 47.f;
      pts[s] = ImVec2(pos.x + pad + u * (cell_w - pad * 2.f),
                      pos.y + cell_h - 14.f - samples[s] * track_h);
    }
    dl->AddPolyline(pts, 48,
                    sel ? fw::theme::U32(fw::theme::kPalette.accent)
                        : fw::theme::U32A(fw::theme::kPalette.accent, 0.7f),
                    0, sel ? 1.6f : 1.2f);

    char label[4];
    std::snprintf(label, sizeof(label), "n%x", i);
    dl->AddText(ImVec2(pos.x + 4.f, pos.y + cell_h - 14.f),
               sel ? fw::theme::U32(fw::theme::kPalette.text)
                   : fw::theme::U32A(fw::theme::kPalette.text_dim, 0.9f),
               label);

    ImGui::PopID();
  }
}

void DrawEnvelopeEditor(App &app)
{
  EnvProgram &prog = app.voice_envs[static_cast<std::size_t>(app.selected_voice & 15)];
  prog.EnsureValid();

  fw::ui::SectionHeader("ENVELOPE");
  ImGui::TextDisabled(
      "Linear segments -> hold at last end -> release to 0. Per-segment k: "
      "rate × (f/C4)^k");

  ImGui::Text("Editing n%x", app.selected_voice & 15);
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

  DrawEnvelopeCurveEditor(prog, ImVec2(-1, 180));

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
    app.bus.QueueChannel([cmd](protocol::ChannelClient &ch) {
      return ch.Exec(cmd);
    });
    app.log.Push(std::string("-> ") + cmd);
    if (app.env_apply_all) {
      // Mirror locally so the all-voices overview reflects what the card
      // now actually holds for every voice.
      for (auto &e : app.voice_envs) {
        e = prog;
      }
    }
  }
  ImGui::EndDisabled();
  if (ImGui::Button("Query enN")) {
    const uint8_t slot = static_cast<uint8_t>(app.selected_voice & 15);
    app.bus.QueueChannel([slot](protocol::ChannelClient &ch) {
      return ch.GetEnvelope(slot);
    });
  }
  ImGui::SameLine();
  if (ImGui::Button("Bulk ekN from preview k")) {
    const uint8_t slot = static_cast<uint8_t>(app.selected_voice & 15);
    const double k = static_cast<double>(pk);
    app.bus.QueueChannel([slot, k](protocol::ChannelClient &ch) {
      return ch.SetEnvK(slot, k);
    });
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
    app.bus.QueueEffect([](protocol::EffectClient &e) { return e.Status(); });
  }
  if (fw::ui::GlowButton("48V phantom ON", ImVec2(-1, 0))) {
    app.bus.QueueEffect([](protocol::EffectClient &e) { return e.Set48V(true); });
  }
  if (fw::ui::GlowButton("48V phantom OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueEffect([](protocol::EffectClient &e) { return e.Set48V(false); });
  }
  if (fw::ui::GlowButton("AUDIO_EN ON", ImVec2(-1, 0))) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetAudioEn(true); });
  }
  if (fw::ui::GlowButton("AUDIO_EN OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetAudioEn(false); });
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("eff_leds", ImVec2(0, 168), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("LEDS");
  if (fw::ui::GlowButton("Auto flash ON", ImVec2(-1, 0))) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedFlash(true); });
  }
  if (fw::ui::GlowButton("Auto flash OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedFlash(false); });
  }
  if (fw::ui::GlowButton("Red ON", ImVec2((ImGui::GetContentRegionAvail().x - 8.f) * 0.5f, 0))) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedRed(true); });
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Red OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedRed(false); });
  }
  if (fw::ui::GlowButton("Yellow ON", ImVec2((ImGui::GetContentRegionAvail().x - 8.f) * 0.5f, 0))) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedYellow(true); });
  }
  ImGui::SameLine();
  if (fw::ui::GlowButton("Yellow OFF", ImVec2(-1, 0), true)) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetLedYellow(false); });
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
      const uint8_t uch = static_cast<uint8_t>(ch);
      app.bus.QueueEffect([uch](protocol::EffectClient &e) {
        return e.SetUsbAdcCh(uch);
      });
    }
  }
  if (ImGui::Button("Query u")) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.GetUsbAdcCh(); });
  }

  ImGui::SeparatorText("Echo / lab shortcuts");
  ImGui::TextDisabled("Keep echo OFF while using MIDI / Control GUI");
  if (ImGui::Button("ec 0 (echo off)")) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetEcho(false); });
  }
  ImGui::SameLine();
  if (ImGui::Button("ec 1 (echo on)")) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.SetEcho(true); });
  }
  if (ImGui::Button("I2C scan")) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.I2cScan(); });
  }
  ImGui::SameLine();
  if (ImGui::Button("ADC init  ai")) {
    app.bus.QueueEffect(
        [](protocol::EffectClient &e) { return e.AdcInit(); });
  }
}
