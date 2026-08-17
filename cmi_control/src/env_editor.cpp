#include "env_editor.hpp"

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

/**
 * Interactive envelope curve (design ENVELOPE EDITOR canvas): grid, filled
 * area, glow line, draggable breakpoints with amp labels. Double-click empty
 * space to split a segment there, right-click a breakpoint to remove it,
 * drag to reshape (vertical = amplitude, horizontal re-solves that segment's
 * slope). Breakpoint 0 (t=0/amp=0) is fixed; the release point's amplitude
 * is pinned to 0 but its time (via slope) is draggable.
 */
void DrawEnvelopeCurveEditor(App &app, EnvProgram &prog, ImVec2 size_arg)
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

  dl->AddRectFilled(p0, p1, fw::theme::U32(kPalette.bg_alt), S(2.f));
  dl->AddRect(p0, p1, fw::theme::U32(kPalette.border), S(2.f));

  ImGui::InvisibleButton("envcurve", size);
  const bool canvas_hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // Grid: 10 columns, 5 rows (design)
  for (int i = 0; i <= 10; ++i) {
    const float x = p0.x + w * (static_cast<float>(i) / 10.f);
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y),
                fw::theme::U32A(kPalette.accent, 0.06f), 1.f);
  }
  for (int i = 1; i <= 5; ++i) {
    const float y = p0.y + h * (static_cast<float>(i) / 5.f);
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y),
                fw::theme::U32A(kPalette.accent, 0.06f), 1.f);
  }

  dl->PushClipRect(p0, p1, true);
  {
    // Filled area under the curve
    ImVec2 fill_pts[258];
    for (int i = 0; i < 256; ++i) {
      const float u = static_cast<float>(i) / 255.f;
      fill_pts[i] = ImVec2(p0.x + u * w, p0.y + h - samples[i] * h);
    }
    fill_pts[256] = ImVec2(p1.x, p1.y);
    fill_pts[257] = ImVec2(p0.x, p1.y);
    dl->AddConcavePolyFilled(fill_pts, 258,
                             fw::theme::U32A(kPalette.accent, 0.05f));

    // Envelope line, glow passes
    ImVec2 pts[256];
    for (int i = 0; i < 256; ++i) {
      pts[i] = fill_pts[i];
    }
    dl->AddPolyline(pts, 256, fw::theme::U32A(kPalette.accent, 0.15f), 0, 5.f);
    dl->AddPolyline(pts, 256, fw::theme::U32(kPalette.accent), 0, 1.5f);
  }
  dl->PopClipRect();

  static int s_drag_idx = -1; // 1..n -> segs[idx-1]; -1 = none

  // Hit-test breakpoints 1..n (0 is the fixed start, not draggable).
  int hovered_handle = -1;
  if (canvas_hovered || s_drag_idx >= 0) {
    const float kPickR = S(12.f);
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

  // Draw breakpoints + amp labels (start point drawn as fixed endpoint dot).
  ImFont *fs = fw::theme::g_fonts.mono_small;
  for (int i = 0; i <= n; ++i) {
    const ImVec2 hp = ScreenOf(i);
    const bool is_end = (i == 0 || i == n);
    const bool active_h = (i == s_drag_idx) || (i == hovered_handle);
    const float r = S(is_end ? 4.f : 5.f) + (active_h ? S(1.5f) : 0.f);
    dl->AddCircleFilled(hp, r,
                        fw::theme::U32(is_end ? kPalette.text_dim
                                              : kPalette.accent),
                        14);
    dl->AddCircle(hp, r, fw::theme::U32(kPalette.bg), 14, 1.5f);
    if (i > 0) {
      char amp[16];
      std::snprintf(amp, sizeof(amp), "%.2f",
                    static_cast<double>(amps[static_cast<std::size_t>(i)]));
      dl->AddText(fs, fs->FontSize, ImVec2(hp.x + S(6.f), hp.y - S(14.f)),
                  fw::theme::U32A(kPalette.accent, 0.6f), amp);
    }
  }

  // Continue an active drag.
  if (s_drag_idx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    const int segi = s_drag_idx - 1;
    const bool is_release = (s_drag_idx == n);
    const float px_per_sec = (total > 1e-6f) ? w / total : 0.f;
    const float dt = (px_per_sec > 1e-6f) ? d.x / px_per_sec : 0.f;
    const float prev_time = times[static_cast<std::size_t>(s_drag_idx - 1)];
    // Firmware allows any slope > 0; 1 ms floor keeps segments distinguishable
    // while still allowing snappy attacks (previously hard-capped at 20 ms).
    constexpr float kMinSeg = 0.001f;
    constexpr float kMaxSlope = 1000.f;
    const float new_time =
        std::max(times[static_cast<std::size_t>(s_drag_idx)] + dt,
                 prev_time + kMinSeg);
    const float prev_amp = amps[static_cast<std::size_t>(s_drag_idx - 1)];
    const float new_amp =
        is_release ? 0.f
                   : std::clamp(amps[static_cast<std::size_t>(s_drag_idx)] - d.y / h, 0.f, 1.f);
    const float new_slope =
        std::fabs(new_amp - prev_amp) / std::max(new_time - prev_time, kMinSeg);
    prog.segs[static_cast<std::size_t>(segi)].end_amp = new_amp;
    prog.segs[static_cast<std::size_t>(segi)].slope =
        std::clamp(new_slope, 0.05f, kMaxSlope);
  } else if (s_drag_idx >= 0) {
    s_drag_idx = -1; // mouse released
  }

  // Right-click a middle breakpoint removes it (merges segments).
  if (canvas_hovered && hovered_handle >= 1 && hovered_handle < n &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    app.env_undo.push_back(prog);
    prog.RemoveSegmentBeforeRelease(hovered_handle - 1);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    return;
  }

  // Start a drag, or add a breakpoint on double-click (design behavior).
  if (canvas_hovered && s_drag_idx < 0 &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hovered_handle >= 0) {
      s_drag_idx = hovered_handle;
    }
  }
  if (canvas_hovered && hovered_handle < 0 &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
      n < EnvProgram::kMaxSegs) {
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
        std::fabs(click_amp - prev_amp) / std::max(click_time - t_k, 0.001f),
        0.05f, 1000.f);
    seg_a.k = prog.segs[static_cast<std::size_t>(k)].k;

    EnvSegment seg_b = prog.segs[static_cast<std::size_t>(k)];
    seg_b.end_amp = orig_end;
    seg_b.slope = std::clamp(
        std::fabs(orig_end - click_amp) / std::max(t_k1 - click_time, 0.001f),
        0.05f, 1000.f);

    app.env_undo.push_back(prog);
    prog.segs[static_cast<std::size_t>(k)] = seg_b;
    prog.segs.insert(prog.segs.begin() + k, seg_a);
  }

  ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
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

/* ── ENVELOPE EDITOR panel ───────────────────────────────────────────── */

void DrawEnvelopeEditor(App &app)
{
  app.selected_voice =
      std::clamp(app.selected_voice, 0,
                 static_cast<int>(cardlink::midi::kVoiceCount) - 1);
  EnvProgram &prog = app.voice_envs[static_cast<std::size_t>(app.selected_voice)];
  prog.EnsureValid();
  const bool offline = !app.bus.IsOpen() || app.bus.BusFault();
  ImFont *fs = fw::theme::g_fonts.mono_small;
  ImFont *fm = fw::theme::g_fonts.mono;

  auto PushUndo = [&] {
    app.env_undo.push_back(prog);
    while (app.env_undo.size() > 32) {
      app.env_undo.erase(app.env_undo.begin());
    }
  };

  fw::ui::BeginSection("env_editor", "ENVELOPE EDITOR", ImVec2(0, 0));
  ImGui::NewLine();
  ImGui::Spacing();

  // ── Voice select: 8 mini cards with envelope thumbnails
  MonoText("VOICE SELECT", kPalette.text_dim, fs);
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float gap = S(4.f);
    const int nvoices = static_cast<int>(cardlink::midi::kVoiceCount);
    const float cw =
        (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(nvoices - 1)) /
        static_cast<float>(nvoices);
    const float ch = S(36.f);
    for (int i = 0; i < nvoices; ++i) {
      if (i > 0) {
        ImGui::SameLine(0.f, gap);
      }
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::PushID(i);
      ImGui::InvisibleButton("##vs", ImVec2(cw, ch));
      if (ImGui::IsItemClicked()) {
        app.selected_voice = i;
      }
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      const bool sel = (app.selected_voice == i);
      const ImVec2 p1(p0.x + cw, p0.y + ch);
      dl->AddRectFilled(p0, p1,
                        sel ? fw::theme::U32A(kPalette.accent, 0.14f)
                            : fw::theme::U32(kPalette.bg_alt),
                        S(2.f));
      dl->AddRect(p0, p1,
                  sel       ? fw::theme::U32A(kPalette.accent, 0.45f)
                  : hovered ? fw::theme::U32(kPalette.border_hi)
                            : fw::theme::U32(kPalette.border),
                  S(2.f));
      char id[4];
      std::snprintf(id, sizeof(id), "n%x", i);
      const ImVec2 idw = fs->CalcTextSizeA(fs->FontSize, FLT_MAX, 0.f, id);
      dl->AddText(fs, fs->FontSize,
                  ImVec2(p0.x + (cw - idw.x) * 0.5f, p0.y + S(2.f)),
                  fw::theme::U32(sel ? kPalette.accent : kPalette.muted), id);
      // Mini envelope path for this voice
      float mini[24];
      app.voice_envs[static_cast<std::size_t>(i)].SampleCurve(mini, 24);
      ImVec2 mp[24];
      const float mh = S(14.f);
      for (int s = 0; s < 24; ++s) {
        const float u = static_cast<float>(s) / 23.f;
        mp[s] = ImVec2(p0.x + S(3.f) + u * (cw - S(6.f)),
                       p0.y + ch - S(3.f) - mini[s] * mh);
      }
      dl->AddPolyline(mp, 24,
                      sel ? fw::theme::U32(kPalette.accent)
                          : fw::theme::U32(kPalette.muted),
                      0, 1.f);
    }
  }
  ImGui::Spacing();

  // ── Presets + undo
  {
    MonoText("Presets:", kPalette.text_dim, fs);
    ImGui::SameLine(0.f, 8.f);
    struct Preset
    {
      const char *name;
      void (EnvProgram::*fn)();
    };
    static const Preset kPresets[] = {
        {"Pluck", &EnvProgram::ResetPluck},
        {"Pad", &EnvProgram::ResetPad},
        {"Organ", &EnvProgram::ResetOrgan},
        {"Snappy", &EnvProgram::ResetSnappy},
    };
    for (const auto &p : kPresets) {
      if (fw::ui::ChipBtn(p.name, false, BtnKind::Neutral)) {
        PushUndo();
        (prog.*(p.fn))();
      }
      ImGui::SameLine(0.f, S(4.f));
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - S(84.f));
    ImGui::BeginDisabled(app.env_undo.empty());
    if (fw::ui::ChipBtn("\u21A9 Undo", false, BtnKind::Neutral)) {
      prog = app.env_undo.back();
      app.env_undo.pop_back();
    }
    ImGui::EndDisabled();
  }
  ImGui::Spacing();

  // ── Curve editor
  DrawEnvelopeCurveEditor(app, prog, ImVec2(-1, S(160.f)));
  {
    char hint[96];
    std::snprintf(hint, sizeof(hint),
                  "Double-click to add · Right-click to remove · Drag to "
                  "reshape · %d/10 segments",
                  static_cast<int>(prog.segs.size()));
    MonoText(hint, kPalette.muted, fs);
  }
  ImGui::Spacing();

  // ── Segment table: # / End amp / Duration / k (pitch)
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float table_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 t0 = ImGui::GetCursorScreenPos();
    const float col0 = S(28.f);
    const float coln = (table_w - col0) / 3.f;
    const float head_h = S(18.f);
    const float row_h = S(22.f);
    // Header
    dl->AddRectFilled(t0, ImVec2(t0.x + table_w, t0.y + head_h),
                      fw::theme::U32(kPalette.bg_alt));
    const char *heads[] = {"#", "End amp", "Duration", "k (pitch)"};
    float hx = t0.x + S(6.f);
    for (int i = 0; i < 4; ++i) {
      dl->AddText(fs, fs->FontSize, ImVec2(hx, t0.y + S(4.f)),
                  fw::theme::U32(kPalette.text_dim), heads[i]);
      hx = t0.x + col0 + coln * static_cast<float>(i) + S(6.f);
    }
    dl->AddLine(ImVec2(t0.x, t0.y + head_h),
                ImVec2(t0.x + table_w, t0.y + head_h),
                fw::theme::U32(kPalette.border));
    ImGui::Dummy(ImVec2(table_w, head_h));

    float start_amp = 0.f;
    for (int i = 0; i < static_cast<int>(prog.segs.size()); ++i) {
      ImGui::PushID(400 + i);
      const bool is_rel = (i + 1 == static_cast<int>(prog.segs.size()));
      EnvSegment &seg = prog.segs[static_cast<std::size_t>(i)];
      const ImVec2 r0 = ImGui::GetCursorScreenPos();

      char idx[8];
      std::snprintf(idx, sizeof(idx), "%d", i);
      dl->AddText(fs, fs->FontSize, ImVec2(r0.x + S(6.f), r0.y + S(5.f)),
                  fw::theme::U32(kPalette.muted), idx);
      char amp[16];
      const float end = is_rel ? 0.f : seg.end_amp;
      std::snprintf(amp, sizeof(amp), "%.3f", static_cast<double>(end));
      dl->AddText(fs, fs->FontSize,
                  ImVec2(r0.x + col0 + S(6.f), r0.y + S(5.f)),
                  fw::theme::U32(kPalette.accent), amp);
      char durs[24];
      const float d_ms =
          EnvProgram::SegDuration(start_amp, seg, is_rel) * 1000.f;
      std::snprintf(durs, sizeof(durs), "~%.0fms", static_cast<double>(d_ms));
      dl->AddText(fs, fs->FontSize,
                  ImVec2(r0.x + col0 + coln + S(6.f), r0.y + S(5.f)),
                  fw::theme::U32(kPalette.text_dim), durs);

      ImGui::SetCursorScreenPos(
          ImVec2(r0.x + col0 + coln * 2.f + S(6.f), r0.y + S(3.f)));
      ImGui::SetNextItemWidth(coln - S(50.f));
      ImGui::SliderFloat("##k", &seg.k, kKMin, kKMax, "");
      ImGui::SameLine(0.f, S(4.f));
      char kv[16];
      std::snprintf(kv, sizeof(kv), "%.1f", static_cast<double>(seg.k));
      MonoText(kv, kPalette.text_dim, fs);

      ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + row_h));
      dl->AddLine(ImVec2(r0.x, r0.y + row_h - 1.f),
                  ImVec2(r0.x + table_w, r0.y + row_h - 1.f),
                  fw::theme::U32(kPalette.border));
      if (!is_rel) {
        start_amp = seg.end_amp;
      }
      ImGui::PopID();
    }
    dl->AddRect(t0, ImVec2(t0.x + table_w, ImGui::GetCursorScreenPos().y),
                fw::theme::U32(kPalette.border), S(2.f));
  }
  ImGui::Spacing();

  // ── Apply row
  {
    const std::string cmd_one = prog.FormatCommand(app.selected_voice);
    const std::string cmd_all = prog.FormatCommand(-1);
    ImGui::BeginDisabled(offline);
    if (fw::ui::Btn("APPLY", S2(76.f, 24.f), BtnKind::Primary)) {
      app.log.Push(LogKind::Tx, "-> " + cmd_one);
      const auto r = app.bus.QueueChannel(
          [cmd_one](cardproto::ChannelClient &ch) { return ch.Exec(cmd_one); });
      app.NotifyEnqueue(r == BusQueueResult::Ok, "bus offline");
    }
    ImGui::SameLine(0.f, S(8.f));
    if (fw::ui::Btn("APPLY ALL 8", S2(116.f, 24.f), BtnKind::Neutral)) {
      app.log.Push(LogKind::Tx, "-> " + cmd_all);
      const auto r = app.bus.QueueChannel(
          [cmd_all](cardproto::ChannelClient &ch) { return ch.Exec(cmd_all); });
      app.NotifyEnqueue(r == BusQueueResult::Ok, "bus offline");
      for (auto &e : app.voice_envs) {
        e = prog;
      }
    }
    ImGui::SameLine(0.f, S(8.f));
    if (fw::ui::Btn("QUERY", S2(76.f, 24.f), BtnKind::Neutral)) {
      const uint8_t slot = static_cast<uint8_t>(app.selected_voice);
      app.bus.QueueChannel([slot](cardproto::ChannelClient &ch) {
        return ch.GetEnvelope(slot);
      });
    }
    ImGui::EndDisabled();
    if (offline) {
      ImGui::SameLine(0.f, S(8.f));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6.f));
      MonoText("Bus offline", kPalette.muted, fs);
    }
  }

  // Command preview (kept from the previous editor — useful when scripting)
  ImGui::Spacing();
  {
    const std::string cmd = prog.FormatCommand(app.selected_voice);
    ImGui::PushFont(fm);
    ImGui::TextColored(kPalette.muted, "%s", cmd.c_str());
    ImGui::PopFont();
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
