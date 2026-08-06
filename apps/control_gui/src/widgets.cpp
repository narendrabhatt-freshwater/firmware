#include "widgets.hpp"

#include "anim.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
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
  if (it == store.end()) {
    return store.emplace(id, init).first->second;
  }
  return it->second;
}

/** Shared sliding-pill implementation behind SegmentedControl and
 * AnimatedTabBar; only sizing differs between the two. */
void PillGroup(const char *str_id, const char *const *labels, int count,
              int *current, float height)
{
  if (count <= 0) {
    return;
  }
  *current = std::clamp(*current, 0, count - 1);

  ImGui::PushID(str_id);
  const ImGuiID gid = ImGui::GetID("##pillgroup");
  static std::unordered_map<ImGuiID, float> s_pill_x;
  static std::unordered_map<ImGuiID, float> s_pill_w;

  std::vector<float> widths(static_cast<std::size_t>(count));
  std::vector<float> xoff(static_cast<std::size_t>(count));
  const float pad = height * 0.42f;
  float total = 0.f;
  for (int i = 0; i < count; ++i) {
    const std::size_t si = static_cast<std::size_t>(i);
    widths[si] = ImGui::CalcTextSize(labels[i]).x + pad * 2.f;
    xoff[si] = total;
    total += widths[si];
  }

  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(pos, V2Add(pos, ImVec2(total, height)),
                    U32A(kPalette.panel_alt, 0.55f), height * 0.5f);

  const float dt = ImGui::GetIO().DeltaTime;
  const float target_x = xoff[static_cast<std::size_t>(*current)];
  const float target_w = widths[static_cast<std::size_t>(*current)];
  float &pill_x = AnimSlot(s_pill_x, gid, target_x);
  float &pill_w = AnimSlot(s_pill_w, gid, target_w);
  pill_x = fw::anim::ExpApproach(pill_x, target_x, dt, 16.f);
  pill_w = fw::anim::ExpApproach(pill_w, target_w, dt, 16.f);

  dl->AddRectFilled(V2Add(pos, ImVec2(pill_x + 3.f, 3.f)),
                    V2Add(pos, ImVec2(pill_x + pill_w - 3.f, height - 3.f)),
                    U32(kPalette.accent), (height - 6.f) * 0.5f);

  for (int i = 0; i < count; ++i) {
    const std::size_t si = static_cast<std::size_t>(i);
    if (i) {
      ImGui::SameLine(0.f, 0.f);
    }
    ImGui::InvisibleButton(labels[i], ImVec2(widths[si], height));
    if (ImGui::IsItemClicked()) {
      *current = i;
    }
    const bool sel = (*current == i);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 tsize = ImGui::CalcTextSize(labels[i]);
    const ImVec2 tpos = V2Add(
        pos, ImVec2(xoff[si] + (widths[si] - tsize.x) * 0.5f,
                    (height - tsize.y) * 0.5f));
    ImU32 tcol;
    if (sel) {
      tcol = U32(kPalette.bg);
    } else if (hovered) {
      tcol = U32(kPalette.text);
    } else {
      tcol = U32(kPalette.text_dim);
    }
    dl->AddText(tpos, tcol, labels[i]);
  }

  ImGui::PopID();
}

} // namespace

void StatusPill(const char *str_id, const char *label, bool active,
                bool alert)
{
  ImGui::PushID(str_id);
  const ImVec2 tsize = ImGui::CalcTextSize(label);
  const float dot_r = 4.5f;
  const float pad_x = 10.f;
  const float pad_y = 6.f;
  const ImVec2 size(dot_r * 2.f + 8.f + tsize.x + pad_x * 2.f,
                    std::max(tsize.y, dot_r * 2.f) + pad_y * 2.f);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("##pill", size);

  const double time = ImGui::GetTime();
  const ImVec4 &dot_base = alert ? kPalette.danger
                            : active ? kPalette.success
                                      : kPalette.text_dim;
  const float pulse = (alert || active)
                          ? fw::anim::Pulse01(time, alert ? 0.55f : 1.6f)
                          : 0.f;

  dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.panel_alt, 0.6f),
                    size.y * 0.5f);

  const ImVec2 dot_center =
      V2Add(pos, ImVec2(pad_x + dot_r, size.y * 0.5f));
  if (active || alert) {
    const float halo_r = dot_r * (1.6f + 0.9f * pulse);
    dl->AddCircleFilled(dot_center, halo_r,
                        U32A(dot_base, (alert ? 0.35f : 0.22f) * (0.5f + 0.5f * pulse)),
                        20);
  }
  dl->AddCircleFilled(dot_center, dot_r, U32(dot_base), 16);

  const ImVec2 tpos =
      V2Add(pos, ImVec2(pad_x + dot_r * 2.f + 8.f, pad_y - 0.5f));
  dl->AddText(tpos, U32(kPalette.text), label);

  ImGui::PopID();
}

void SegmentedControl(const char *str_id, const char *const *labels,
                      int count, int *current)
{
  PillGroup(str_id, labels, count, current, 28.f);
}

void SectionHeader(const char *title, const char *pill_id,
                   const char *pill_label, bool active, bool alert)
{
  ImGui::TextUnformatted(title);
  if (!pill_id || !pill_label) {
    return;
  }
  ImGui::SameLine();
  const float pill_w = ImGui::CalcTextSize(pill_label).x + 46.f;
  const float avail = ImGui::GetContentRegionAvail().x;
  if (avail > pill_w) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - pill_w);
  }
  StatusPill(pill_id, pill_label, active, alert);
}

void PianoKey(const char *str_id, const char *label, const ImVec2 &size,
             bool sounding)
{
  ImGui::PushID(str_id);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("##key", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();

  const ImGuiID id = ImGui::GetID("##key");
  static std::unordered_map<ImGuiID, float> s_press;
  float &press = AnimSlot(s_press, id, 0.f);
  const float dt = ImGui::GetIO().DeltaTime;
  const float target = (held || sounding) ? 1.f : 0.f;
  press = std::clamp(
      fw::anim::ExpApproach(press, target, dt, held ? 34.f : 16.f), 0.f, 1.f);

  const ImVec4 ivory(0.88f, 0.89f, 0.91f, 1.f);
  const ImVec4 fill(ivory.x + (kPalette.accent.x - ivory.x) * press,
                    ivory.y + (kPalette.accent.y - ivory.y) * press,
                    ivory.z + (kPalette.accent.z - ivory.z) * press, 1.f);

  const float dip = press * 3.f;
  const ImVec2 p0 = V2Add(pos, ImVec2(2.f, dip));
  const ImVec2 p1 = V2Add(pos, ImVec2(size.x - 2.f, size.y));

  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(fill), 5.f,
                    ImDrawFlags_RoundCornersBottom);
  dl->AddRectFilled(
      p0, ImVec2(p1.x, p0.y + size.y * 0.14f),
      ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.16f * (1.f - press))), 5.f,
      ImDrawFlags_RoundCornersTop);
  dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.35f)), 5.f,
             ImDrawFlags_RoundCornersBottom, 1.f);
  if (hovered && press < 0.05f) {
    dl->AddRect(p0, p1, ImGui::GetColorU32(kPalette.accent_dim), 5.f,
               ImDrawFlags_RoundCornersBottom, 1.5f);
  }

  const ImVec2 tsize = ImGui::CalcTextSize(label);
  const ImVec2 tpos =
      V2Add(pos, ImVec2((size.x - tsize.x) * 0.5f,
                        size.y - tsize.y - 8.f + dip));
  const ImVec4 text_col =
      press > 0.55f ? ImVec4(1.f, 1.f, 1.f, 1.f) : ImVec4(0.15f, 0.16f, 0.20f, 1.f);
  dl->AddText(tpos, ImGui::GetColorU32(text_col), label);

  ImGui::PopID();
}

void AnimatedTabBar(const char *str_id, const char *const *labels, int count,
                    int *current)
{
  PillGroup(str_id, labels, count, current, 36.f);
}

void LevelMeter(const char *str_id, float value01, const ImVec2 &size_arg)
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

  dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.panel_alt, 0.7f),
                    size.y * 0.3f);

  const float fillw = size.x * v;
  if (fillw > 0.5f) {
    const ImU32 c_hot = v > 0.85f ? U32(kPalette.danger)
                        : v > 0.6f ? U32(kPalette.warning)
                                   : U32(kPalette.accent);
    dl->AddRectFilledMultiColor(pos, V2Add(pos, ImVec2(fillw, size.y)),
                                U32(kPalette.accent), c_hot, c_hot,
                                U32(kPalette.accent));
    dl->AddRectFilled(V2Add(pos, ImVec2(fillw - 2.f, 0.f)),
                      V2Add(pos, ImVec2(fillw + 2.f, size.y)),
                      U32A(kPalette.text, 0.30f));
  }
  dl->AddRect(pos, V2Add(pos, size), U32A(kPalette.border, 0.8f),
             size.y * 0.3f);

  ImGui::PopID();
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

  dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.panel_alt, 0.55f),
                    8.f);
  dl->AddRect(pos, V2Add(pos, size), U32A(kPalette.border, 0.7f), 8.f);

  const float midy = pos.y + size.y * 0.5f;
  dl->AddLine(ImVec2(pos.x + 6.f, midy), ImVec2(pos.x + size.x - 6.f, midy),
             U32A(kPalette.border, 0.5f), 1.f);

  if (count >= 2 && samples) {
    dl->PushClipRect(pos, V2Add(pos, size), true);
    std::vector<ImVec2> pts(static_cast<std::size_t>(count));
    const float range = scale_max - scale_min;
    const float inv_range = (std::fabs(range) > 1e-6f) ? 1.f / range : 1.f;
    for (int i = 0; i < count; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(count - 1);
      const float t = (samples[i] - scale_min) * inv_range;
      pts[static_cast<std::size_t>(i)] =
          ImVec2(pos.x + u * size.x, pos.y + size.y - t * size.y);
    }
    dl->AddPolyline(pts.data(), count, U32A(kPalette.accent, 0.10f), 0, 6.f);
    dl->AddPolyline(pts.data(), count, U32A(kPalette.accent, 0.24f), 0, 3.5f);
    dl->AddPolyline(pts.data(), count, U32(kPalette.accent), 0, 1.6f);
    dl->PopClipRect();
  }

  ImGui::PopID();
}

bool GlowButton(const char *label, const ImVec2 &size_arg, bool danger)
{
  ImGui::PushID(label);
  const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
  const ImVec2 padding(16.f, 10.f);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImVec2 size = size_arg;
  if (size.x == 0.f) {
    size.x = label_size.x + padding.x * 2.f;
  } else if (size.x < 0.f) {
    size.x = avail.x;
  }
  if (size.y == 0.f) {
    size.y = label_size.y + padding.y * 2.f;
  } else if (size.y < 0.f) {
    size.y = avail.y;
  }

  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("##glowbtn", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  const bool pressed = ImGui::IsItemClicked();

  const ImGuiID id = ImGui::GetID("##glowbtn");
  static std::unordered_map<ImGuiID, float> s_glow;
  float &glow = AnimSlot(s_glow, id, 0.f);
  const float dt = ImGui::GetIO().DeltaTime;
  glow = fw::anim::ExpApproach(glow, hovered ? 1.f : 0.f, dt, 12.f);

  const ImVec4 &base = danger ? kPalette.danger : kPalette.accent;
  const float inset = active ? 2.f : 0.f;
  const ImVec2 p0 = V2Add(pos, ImVec2(inset, inset));
  const ImVec2 p1 = V2Add(pos, ImVec2(size.x - inset, size.y - inset));

  // GetColorU32() (rather than the raw U32/U32A helpers) folds in the
  // current style.Alpha, so BeginDisabled()'s dimming still applies even
  // though this widget draws itself via ImDrawList.
  const float fill_a = active ? 0.85f : (0.30f + 0.45f * glow);
  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, fill_a)), 6.f);
  dl->AddRect(p0, p1,
             ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, 0.55f + 0.45f * glow)),
             6.f, 0, 1.5f);

  const ImVec4 &text_col =
      (active || glow > 0.5f) ? kPalette.bg : kPalette.text;
  const ImVec2 tsize = ImGui::CalcTextSize(label);
  const ImVec2 tpos = V2Add(
      pos, ImVec2((size.x - tsize.x) * 0.5f, (size.y - tsize.y) * 0.5f));
  dl->AddText(tpos, ImGui::GetColorU32(text_col), label);

  ImGui::PopID();
  return pressed;
}

void VoiceSlotBadge(const char *str_id, const char *label, bool active,
                    float glow01, const char *sub)
{
  ImGui::PushID(str_id);
  const ImVec2 size(58.f, 42.f);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("##slot", size);

  const float g = std::clamp(glow01, 0.f, 1.f);
  dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.panel_alt, 0.9f),
                    8.f);
  if (g > 0.01f) {
    dl->AddRectFilled(pos, V2Add(pos, size), U32A(kPalette.accent, 0.28f * g),
                      8.f);
  }
  dl->AddRect(pos, V2Add(pos, size),
             g > 0.01f ? U32A(kPalette.accent, 0.4f + 0.6f * g)
                       : U32A(kPalette.border, 0.6f),
             8.f, 0, 1.f + g);

  dl->AddText(V2Add(pos, ImVec2(8.f, 5.f)),
             active ? U32(kPalette.text) : U32A(kPalette.text_dim, 0.85f),
             label);
  if (sub) {
    dl->AddText(V2Add(pos, ImVec2(8.f, 22.f)), U32A(kPalette.text_dim, 0.9f),
               sub);
  }

  ImGui::PopID();
}

} // namespace fw::ui
