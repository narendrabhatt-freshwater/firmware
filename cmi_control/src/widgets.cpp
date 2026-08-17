#include "widgets.hpp"

#include "anim.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace fw::ui
{

  namespace
  {

    using theme::kPalette;
    using theme::U32;
    using theme::U32A;

    ImVec2 V2Add(const ImVec2 &a, const ImVec2 &b)
    {
      return ImVec2(a.x + b.x, a.y + b.y);
    }

    float &AnimSlot(std::unordered_map<ImGuiID, float> &store, ImGuiID id,
                    float init)
    {
      auto it = store.find(id);
      if (it == store.end())
      {
        return store.emplace(id, init).first->second;
      }
      return it->second;
    }

    struct BtnColors
    {
      ImVec4 bg;
      ImVec4 border;
      ImVec4 text;
    };

    BtnColors KindColors(BtnKind kind)
    {
      switch (kind)
      {
      case BtnKind::Primary:
        return {ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z,
                       0.10f),
                ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z,
                       0.25f),
                kPalette.accent};
      case BtnKind::Danger:
        return {ImVec4(kPalette.danger.x, kPalette.danger.y, kPalette.danger.z,
                       0.08f),
                ImVec4(kPalette.danger.x, kPalette.danger.y, kPalette.danger.z,
                       0.25f),
                kPalette.danger};
      case BtnKind::Warn:
        return {ImVec4(kPalette.warning.x, kPalette.warning.y,
                       kPalette.warning.z, 0.08f),
                ImVec4(kPalette.warning.x, kPalette.warning.y,
                       kPalette.warning.z, 0.25f),
                kPalette.warning};
      case BtnKind::Neutral:
      default:
        return {kPalette.panel_alt,
                kPalette.border,
                kPalette.text_dim};
      }
    }

    bool ButtonImpl(const char *label, ImVec2 size, BtnKind kind,
                    ImFont *font, const ImVec2 &padding)
    {
      ImGui::PushID(label);
      if (font)
      {
        ImGui::PushFont(font);
      }
      // ImGui "##id" suffixes size the hit target but must not paint.
      const char *text_end = label;
      while (*text_end && !(text_end[0] == '#' && text_end[1] == '#')) {
        ++text_end;
      }
      const ImVec2 label_size = ImGui::CalcTextSize(label, text_end);
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      if (size.x == 0.f)
      {
        size.x = label_size.x + padding.x * 2.f;
      }
      else if (size.x < 0.f)
      {
        size.x = avail.x;
      }
      if (size.y == 0.f)
      {
        size.y = label_size.y + padding.y * 2.f;
      }
      else if (size.y < 0.f)
      {
        size.y = avail.y;
      }

      const ImVec2 pos = ImGui::GetCursorScreenPos();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImGui::InvisibleButton("##btn", size);
      const bool hovered = ImGui::IsItemHovered();
      const bool active = ImGui::IsItemActive();
      const bool pressed = ImGui::IsItemClicked();

      BtnColors bc = KindColors(kind);
      float bg_a = bc.bg.w;
      float border_a = bc.border.w;
      if (hovered)
      {
        bg_a = std::min(1.f, bg_a + 0.07f);
        border_a = std::min(1.f, border_a + 0.20f);
      }
      // GetColorU32 folds style.Alpha so BeginDisabled dimming applies.
      const ImVec2 p0 = active ? V2Add(pos, ImVec2(0.5f, 0.5f)) : pos;
      const ImVec2 p1 = V2Add(pos, active ? ImVec2(size.x - 0.5f, size.y - 0.5f)
                                          : size);
      dl->AddRectFilled(p0, p1,
                        ImGui::GetColorU32(ImVec4(bc.bg.x, bc.bg.y, bc.bg.z,
                                                  bg_a)),
                        theme::S(2.f));
      dl->AddRect(p0, p1,
                  ImGui::GetColorU32(ImVec4(bc.border.x, bc.border.y,
                                            bc.border.z, border_a)),
                  theme::S(2.f));

      const ImVec2 tpos = V2Add(
          pos, ImVec2((size.x - label_size.x) * 0.5f,
                      (size.y - label_size.y) * 0.5f));
      dl->AddText(tpos, ImGui::GetColorU32(bc.text), label, text_end);

      if (font)
      {
        ImGui::PopFont();
      }
      ImGui::PopID();
      return pressed;
    }

  } // namespace

  bool Btn(const char *label, const ImVec2 &size, BtnKind kind)
  {
    return ButtonImpl(label, size, kind, theme::g_fonts.caps,
                      theme::S2(12.f, 6.f));
  }

  bool ChipBtn(const char *label, bool selected, BtnKind kind)
  {
    BtnKind k = kind;
    if (selected)
    {
      k = BtnKind::Primary;
    }
    return ButtonImpl(label, ImVec2(0, 0), k, theme::g_fonts.mono_small,
                      theme::S2(7.f, 3.f));
  }

  bool GlowButton(const char *label, const ImVec2 &size, bool danger)
  {
    return Btn(label, size, danger ? BtnKind::Danger : BtnKind::Primary);
  }

  bool ToggleSwitch(const char *str_id, bool *value, bool enabled)
  {
    ImGui::PushID(str_id);
    const ImVec2 size = theme::S2(32.f, 16.f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##switch", size);
    bool changed = false;
    if (enabled && ImGui::IsItemClicked())
    {
      *value = !*value;
      changed = true;
    }

    const ImGuiID id = ImGui::GetID("##switch");
    static std::unordered_map<ImGuiID, float> s_anim;
    float &t = AnimSlot(s_anim, id, *value ? 1.f : 0.f);
    t = fw::anim::ExpApproach(t, *value ? 1.f : 0.f,
                              ImGui::GetIO().DeltaTime, 18.f);

    const ImVec4 fill = *value
                            ? ImVec4(kPalette.accent.x, kPalette.accent.y,
                                     kPalette.accent.z, 0.25f)
                            : kPalette.bg_alt;
    const ImVec4 border = *value
                              ? ImVec4(kPalette.accent.x, kPalette.accent.y,
                                       kPalette.accent.z, 0.45f)
                              : kPalette.border_hi;
    dl->AddRectFilled(pos, V2Add(pos, size), ImGui::GetColorU32(fill),
                      size.y * 0.5f);
    dl->AddRect(pos, V2Add(pos, size), ImGui::GetColorU32(border),
                size.y * 0.5f);

    const float knob_x = pos.x + theme::S(3.f) + t * theme::S(14.f);
    const float knob_y = pos.y + size.y * 0.5f;
    const ImVec4 knob = *value ? kPalette.accent : kPalette.muted;
    if (*value)
    {
      dl->AddCircleFilled(ImVec2(knob_x + theme::S(5.f), knob_y),
                          theme::S(8.f),
                          ImGui::GetColorU32(ImVec4(kPalette.accent.x,
                                                    kPalette.accent.y,
                                                    kPalette.accent.z, 0.20f)),
                          16);
    }
    dl->AddCircleFilled(ImVec2(knob_x + theme::S(5.f), knob_y), theme::S(5.f),
                        ImGui::GetColorU32(knob), 16);
    ImGui::PopID();
    return changed;
  }

  bool ToggleRow(const char *label, bool *value, bool enabled)
  {
    ImGui::PushID(label);
    const float row_h = theme::S(22.f);
    const float start_y = ImGui::GetCursorPosY();
    bool changed = ToggleSwitch("##sw", value, enabled);
    ImGui::SameLine(0.f, theme::S(10.f));
    ImGui::SetCursorPosY(start_y + theme::S(1.f));
    if (theme::g_fonts.mono)
    {
      ImGui::PushFont(theme::g_fonts.mono);
    }
    ImGui::TextColored(*value ? kPalette.text : kPalette.text_dim, "%s",
                       label);
    if (theme::g_fonts.mono)
    {
      ImGui::PopFont();
    }
    // Right-aligned ON / OFF readout
    ImGui::SameLine();
    const char *state = *value ? "ON" : "OFF";
    if (theme::g_fonts.mono_small)
    {
      ImGui::PushFont(theme::g_fonts.mono_small);
    }
    const float w = ImGui::CalcTextSize(state).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w)
    {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    }
    ImGui::SetCursorPosY(start_y + theme::S(3.f));
    ImGui::TextColored(*value ? kPalette.accent : kPalette.muted, "%s", state);
    if (theme::g_fonts.mono_small)
    {
      ImGui::PopFont();
    }
    // Bottom hairline like the design's row divider
    ImGui::SetCursorPosY(start_y + row_h);
    ImGui::Separator();
    ImGui::PopID();
    return changed;
  }

  void StatusDot(float radius, const ImVec4 &color, bool glow)
  {
    radius = theme::S(radius);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float line_h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + radius, p.y + line_h * 0.5f);
    if (glow)
    {
      dl->AddCircleFilled(c, radius * 2.2f, U32A(color, 0.25f), 16);
    }
    dl->AddCircleFilled(c, radius, U32(color), 12);
    ImGui::Dummy(ImVec2(radius * 2.f, line_h));
  }

  void StatusPill(const char *str_id, const char *label, bool active,
                  bool alert)
  {
    ImGui::PushID(str_id);
    if (theme::g_fonts.mono_small)
    {
      ImGui::PushFont(theme::g_fonts.mono_small);
    }
    const ImVec2 tsize = ImGui::CalcTextSize(label);
    const float dot_r = theme::S(3.f);
    const float pad_x = theme::S(8.f);
    const float pad_y = theme::S(4.f);
    const float gap = theme::S(6.f);
    const ImVec2 size(dot_r * 2.f + gap + tsize.x + pad_x * 2.f,
                      std::max(tsize.y, dot_r * 2.f) + pad_y * 2.f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##pill", size);

    const ImVec4 &dot = alert    ? kPalette.danger
                        : active ? kPalette.accent
                                 : kPalette.muted;
    dl->AddRectFilled(pos, V2Add(pos, size), U32(kPalette.bg_alt),
                      theme::S(2.f));
    dl->AddRect(pos, V2Add(pos, size), U32(kPalette.border), theme::S(2.f));

    const ImVec2 dc = V2Add(pos, ImVec2(pad_x + dot_r, size.y * 0.5f));
    if (active || alert)
    {
      dl->AddCircleFilled(dc, dot_r * 2.f, U32A(dot, 0.30f), 14);
    }
    dl->AddCircleFilled(dc, dot_r, U32(dot), 12);
    dl->AddText(V2Add(pos, ImVec2(pad_x + dot_r * 2.f + gap, pad_y)),
                U32(active || alert ? kPalette.text : kPalette.text_dim),
                label);
    if (theme::g_fonts.mono_small)
    {
      ImGui::PopFont();
    }
    ImGui::PopID();
  }

  bool BeginSection(const char *str_id, const char *title, const ImVec2 &size,
                    bool border)
  {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPalette.panel);
    const ImGuiChildFlags flags =
        border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::S2(10.f, 8.f));
    const bool open = ImGui::BeginChild(str_id, size, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (open && title && title[0])
    {
      // Title row: small mono, letterspaced feel, bottom hairline.
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 ws = ImGui::GetWindowSize();
      if (theme::g_fonts.mono_small)
      {
        ImGui::PushFont(theme::g_fonts.mono_small);
      }
      ImGui::TextColored(kPalette.text_dim, "%s", title);
      if (theme::g_fonts.mono_small)
      {
        ImGui::PopFont();
      }
      const float line_y = wp.y + theme::S(26.f);
      dl->AddLine(ImVec2(wp.x, line_y), ImVec2(wp.x + ws.x, line_y),
                  U32(kPalette.border), 1.f);
      // Cursor stays on the title line so callers can SameLine() actions,
      // then call NewLine()/Spacing before the body.
      ImGui::SameLine();
    }
    return open;
  }

  void EndSection() { ImGui::EndChild(); }

  void EmptyState(const char *title, const char *detail)
  {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 tsize = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         std::max(0.f, (avail.y - tsize.y * 3.f) * 0.35f));
    const float x = std::max(0.f, (avail.x - tsize.x) * 0.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
    ImGui::PushFont(theme::g_fonts.large);
    ImGui::TextColored(kPalette.text_dim, "%s", title);
    ImGui::PopFont();
    if (detail && detail[0]) {
      const ImVec2 dsize = ImGui::CalcTextSize(detail, nullptr, false, avail.x * 0.7f);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                           std::max(0.f, (avail.x - dsize.x) * 0.5f));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x * 0.7f);
      ImGui::TextDisabled("%s", detail);
      ImGui::PopTextWrapPos();
    }
  }

  void GlowWaveform(const char *str_id, const float *samples, int count,
                    const ImVec2 &size_arg, float scale_min, float scale_max)
  {
    ImGui::PushID(str_id);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(size_arg.x <= 0.f ? avail.x + size_arg.x : size_arg.x,
                      size_arg.y <= 0.f ? avail.y + size_arg.y : size_arg.y);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##wave", size);

    dl->AddRectFilled(pos, V2Add(pos, size), U32(kPalette.bg));

    // 10×8 graticule (design Oscilloscope): faint grid, brighter centre axes,
    // minor ticks on the axes.
    const int divx = 10;
    const int divy = 8;
    const ImU32 grid = U32A(kPalette.accent, 0.07f);
    const ImU32 axis = U32A(kPalette.accent, 0.14f);
    for (int i = 0; i <= divx; ++i)
    {
      const float x = pos.x + size.x * (static_cast<float>(i) / divx);
      dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), grid, 1.f);
    }
    for (int i = 0; i <= divy; ++i)
    {
      const float y = pos.y + size.y * (static_cast<float>(i) / divy);
      dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), grid, 1.f);
    }
    const float midy = pos.y + size.y * 0.5f;
    const float midx = pos.x + size.x * 0.5f;
    dl->AddLine(ImVec2(pos.x, midy), ImVec2(pos.x + size.x, midy), axis, 1.f);
    dl->AddLine(ImVec2(midx, pos.y), ImVec2(midx, pos.y + size.y), axis, 1.f);
    constexpr int kMinor = 5;
    for (int i = 0; i < divx; ++i)
    {
      for (int j = 1; j < kMinor; ++j)
      {
        const float x = pos.x + size.x * ((static_cast<float>(i) +
                                           static_cast<float>(j) / kMinor) /
                                          divx);
        dl->AddLine(ImVec2(x, pos.y + size.y * 0.46f),
                    ImVec2(x, pos.y + size.y * 0.54f), grid, 0.5f);
      }
    }
    for (int i = 0; i < divy; ++i)
    {
      for (int j = 1; j < kMinor; ++j)
      {
        const float y = pos.y + size.y * ((static_cast<float>(i) +
                                           static_cast<float>(j) / kMinor) /
                                          divy);
        dl->AddLine(ImVec2(pos.x + size.x * 0.46f, y),
                    ImVec2(pos.x + size.x * 0.54f, y), grid, 0.5f);
      }
    }

    if (count >= 2 && samples)
    {
      dl->PushClipRect(pos, V2Add(pos, size), true);
      std::vector<ImVec2> pts(static_cast<std::size_t>(count));
      const float range = scale_max - scale_min;
      const float inv_range = (std::fabs(range) > 1e-6f) ? 1.f / range : 1.f;
      bool flat = true;
      for (int i = 0; i < count; ++i)
      {
        if (std::fabs(samples[i]) > 1e-6f)
        {
          flat = false;
        }
        const float u = static_cast<float>(i) / static_cast<float>(count - 1);
        const float t = (samples[i] - scale_min) * inv_range;
        pts[static_cast<std::size_t>(i)] =
            ImVec2(pos.x + u * size.x, pos.y + size.y - t * size.y);
      }
      const ImVec4 &trace = kPalette.scope_trace;
      if (flat)
      {
        // Idle: single dim zero line with light glow (design flat trace).
        dl->AddLine(ImVec2(pos.x, midy), ImVec2(pos.x + size.x, midy),
                    U32A(trace, 0.10f), 4.f);
        dl->AddLine(ImVec2(pos.x, midy), ImVec2(pos.x + size.x, midy),
                    U32A(trace, 0.25f), 1.f);
      }
      else
      {
        dl->AddPolyline(pts.data(), count, U32A(trace, 0.18f), 0, 6.f);
        dl->AddPolyline(pts.data(), count, U32A(trace, 0.45f), 0, 3.f);
        dl->AddPolyline(pts.data(), count, U32(trace), 0, 1.2f);
        dl->AddPolyline(pts.data(), count, U32(kPalette.accent_bright), 0,
                        0.5f);
      }
      dl->PopClipRect();
    }

    ImGui::PopID();
  }

  void LevelMeter(const char *str_id, float value01, const ImVec2 &size_arg,
                  float peak_hold01)
  {
    ImGui::PushID(str_id);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(size_arg.x <= 0.f ? avail.x + size_arg.x : size_arg.x,
                      size_arg.y <= 0.f ? avail.y + size_arg.y : size_arg.y);
    const ImGuiID id = ImGui::GetID("##meter");
    static std::unordered_map<ImGuiID, float> s_val;
    const float target = std::clamp(value01, 0.f, 1.f);
    float &v = AnimSlot(s_val, id, target);
    const float dt = ImGui::GetIO().DeltaTime;
    const float speed = (target > v) ? 28.f : 7.f;
    v = std::clamp(fw::anim::ExpApproach(v, target, dt, speed), 0.f, 1.f);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##meterbox", size);

    dl->AddRectFilled(pos, V2Add(pos, size), U32(kPalette.border), 2.f);
    const float fillw = size.x * v;
    if (fillw > 0.5f)
    {
      const ImU32 c = v > 0.9f ? U32(kPalette.danger) : U32(kPalette.accent);
      dl->AddRectFilled(pos, V2Add(pos, ImVec2(fillw, size.y)), c, 2.f);
    }
    if (peak_hold01 >= 0.f) {
      const float px =
          pos.x + size.x * std::clamp(peak_hold01, 0.f, 1.f);
      dl->AddLine(ImVec2(px, pos.y + 1.f), ImVec2(px, pos.y + size.y - 1.f),
                  U32A(kPalette.text, 0.85f), 2.f);
    }
    ImGui::PopID();
  }

  void VerticalMeter(const char *str_id, float value01, const ImVec2 &size,
                     float peak_hold01)
  {
    ImGui::PushID(str_id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##vmeter", size);
    const float v = std::clamp(value01, 0.f, 1.f);
    dl->AddRectFilled(pos, V2Add(pos, size), U32(kPalette.border), 2.f);
    const float fill_h = size.y * v;
    if (fill_h > 1.f) {
      const ImU32 c =
          v > 0.9f ? U32(kPalette.danger) : U32(kPalette.accent);
      dl->AddRectFilled(ImVec2(pos.x, pos.y + size.y - fill_h),
                        ImVec2(pos.x + size.x, pos.y + size.y), c, 2.f);
      dl->AddRectFilled(ImVec2(pos.x - 1.f, pos.y + size.y - fill_h),
                        ImVec2(pos.x + size.x + 1.f, pos.y + size.y),
                        U32A(kPalette.accent, 0.15f), 2.f);
    }
    if (peak_hold01 >= 0.f) {
      const float py =
          pos.y + size.y * (1.f - std::clamp(peak_hold01, 0.f, 1.f));
      dl->AddLine(ImVec2(pos.x, py), ImVec2(pos.x + size.x, py),
                  U32A(kPalette.text, 0.9f), 1.5f);
    }
    ImGui::PopID();
  }

  void ProgressBar(const char *str_id, float value01, const ImVec2 &size)
  {
    ImGui::PushID(str_id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    if (sz.x <= 0.f)
    {
      sz.x = ImGui::GetContentRegionAvail().x;
    }
    if (sz.y <= 0.f)
    {
      sz.y = theme::S(3.f);
    }
    ImGui::InvisibleButton("##bar", sz);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float v = std::clamp(value01, 0.f, 1.f);
    dl->AddRectFilled(pos, V2Add(pos, sz), U32(kPalette.border), sz.y * 0.5f);
    if (v > 0.001f)
    {
      dl->AddRectFilled(pos, V2Add(pos, ImVec2(sz.x * v, sz.y)),
                        U32(kPalette.accent), sz.y * 0.5f);
    }
    ImGui::PopID();
  }

  float Splitter(const char *str_id, bool horizontal_bar, float thickness,
                 float cross_axis_size)
  {
    ImGui::PushID(str_id);
    if (thickness <= 0.f) {
      thickness = theme::S(theme::Metrics::Splitter);
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float long_axis =
        cross_axis_size > 0.f
            ? cross_axis_size
            : (horizontal_bar ? avail.x : avail.y);
    const ImVec2 size =
        horizontal_bar ? ImVec2(long_axis, thickness) : ImVec2(thickness, long_axis);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##split", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered || held) {
      ImGui::SetMouseCursor(horizontal_bar ? ImGuiMouseCursor_ResizeNS
                                           : ImGuiMouseCursor_ResizeEW);
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float a = held ? 0.6f : (hovered ? 0.35f : 0.12f);
    dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.accent, a), 1.f);

    float delta = 0.f;
    if (held) {
      delta = horizontal_bar ? ImGui::GetIO().MouseDelta.y
                             : ImGui::GetIO().MouseDelta.x;
    }
    ImGui::PopID();
    return delta;
  }

  void DrawNavIcon(ImDrawList *dl, NavIcon icon, ImVec2 c, float h, ImU32 col)
  {
    switch (icon)
    {
    case NavIcon::Perform:
    {
      // ◈ outline diamond with filled centre
      const ImVec2 pts[4] = {ImVec2(c.x, c.y - h), ImVec2(c.x + h, c.y),
                             ImVec2(c.x, c.y + h), ImVec2(c.x - h, c.y)};
      dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.2f);
      const float ih = h * 0.45f;
      const ImVec2 ipts[4] = {ImVec2(c.x, c.y - ih), ImVec2(c.x + ih, c.y),
                              ImVec2(c.x, c.y + ih), ImVec2(c.x - ih, c.y)};
      dl->AddConvexPolyFilled(ipts, 4, col);
      break;
    }
    case NavIcon::Tone:
    {
      // ◇ outline diamond
      const ImVec2 pts[4] = {ImVec2(c.x, c.y - h), ImVec2(c.x + h, c.y),
                             ImVec2(c.x, c.y + h), ImVec2(c.x - h, c.y)};
      dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.2f);
      break;
    }
    case NavIcon::Sample:
    {
      // ⊟ boxed minus
      dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col,
                  1.f, 0, 1.2f);
      dl->AddLine(ImVec2(c.x - h * 0.5f, c.y), ImVec2(c.x + h * 0.5f, c.y),
                  col, 1.2f);
      break;
    }
    case NavIcon::Effect:
    {
      // ⊞ boxed plus
      dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col,
                  1.f, 0, 1.2f);
      dl->AddLine(ImVec2(c.x - h * 0.5f, c.y), ImVec2(c.x + h * 0.5f, c.y),
                  col, 1.2f);
      dl->AddLine(ImVec2(c.x, c.y - h * 0.5f), ImVec2(c.x, c.y + h * 0.5f),
                  col, 1.2f);
      break;
    }
    case NavIcon::Setup:
    {
      // ◧ square, left half filled
      dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col,
                  1.f, 0, 1.2f);
      dl->AddRectFilled(ImVec2(c.x - h + 1.5f, c.y - h + 1.5f),
                        ImVec2(c.x, c.y + h - 1.5f), col);
      break;
    }
    }
  }

  bool RefreshBtn(const char *str_id, const ImVec2 &size_arg)
  {
    ImGui::PushID(str_id);
    const ImVec2 size(size_arg.x > 0.f ? size_arg.x : theme::S(26.f),
                      size_arg.y > 0.f ? size_arg.y : theme::S(22.f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##refresh", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const ImVec2 p1(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, p1, ImGui::GetColorU32(kPalette.panel_alt),
                      theme::S(2.f));
    dl->AddRect(pos, p1,
                ImGui::GetColorU32(hovered ? kPalette.border_hi
                                           : kPalette.border),
                theme::S(2.f));
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const float r = std::min(size.x, size.y) * 0.28f;
    const ImU32 col = ImGui::GetColorU32(kPalette.text_dim);
    // 300° arc + arrowhead at the open end.
    dl->PathArcTo(c, r, -0.35f * 3.14159f, 1.25f * 3.14159f, 20);
    dl->PathStroke(col, 0, theme::S(1.4f));
    const float ang = -0.35f * 3.14159f;
    const float ah = theme::S(2.5f);
    const ImVec2 tip(c.x + r * std::cos(ang), c.y + r * std::sin(ang));
    dl->AddTriangleFilled(ImVec2(tip.x - ah, tip.y - ah),
                          ImVec2(tip.x + ah * 1.2f, tip.y),
                          ImVec2(tip.x - ah, tip.y + ah), col);
    ImGui::PopID();
    return clicked;
  }

  void WarnIcon(const ImVec4 &col, float size)
  {
    size = theme::S(size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float line_h = ImGui::GetTextLineHeight();
    const float top = p.y + (line_h - size) * 0.5f;
    const ImVec2 a(p.x + size * 0.5f, top);
    const ImVec2 b(p.x, top + size);
    const ImVec2 c(p.x + size, top + size);
    dl->AddTriangle(a, b, c, U32(col), 1.4f);
    // exclamation tick
    const float cx = p.x + size * 0.5f;
    dl->AddLine(ImVec2(cx, top + size * 0.35f), ImVec2(cx, top + size * 0.68f),
                U32(col), 1.4f);
    dl->AddCircleFilled(ImVec2(cx, top + size * 0.82f), 0.8f, U32(col), 6);
    ImGui::Dummy(ImVec2(size, line_h));
  }

} // namespace fw::ui
