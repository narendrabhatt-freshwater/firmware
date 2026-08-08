#pragma once

#include "imgui.h"

/** Freshwater Control visual identity: dark/aqua palette, DPI-aware spacing
 * tokens, and the Roboto fonts bundled with the vendored Dear ImGui source. */
namespace fw::theme
{

struct Palette
{
  ImVec4 bg;
  ImVec4 bg_alt;
  ImVec4 panel;
  ImVec4 panel_alt;
  ImVec4 border;
  ImVec4 text;
  ImVec4 text_dim;
  ImVec4 accent;
  ImVec4 accent_dim;
  ImVec4 success;
  ImVec4 warning;
  ImVec4 danger;
};

extern const Palette kPalette;

struct Fonts
{
  ImFont *body = nullptr;
  ImFont *large = nullptr;
  ImFont *hero = nullptr;
};

extern Fonts g_fonts;

/** Logical (unscaled) spacing on a 4 px grid. Multiply with Scale(). */
struct Metrics
{
  static constexpr float SpaceXS = 4.f;
  static constexpr float SpaceS = 8.f;
  static constexpr float SpaceM = 12.f;
  static constexpr float SpaceL = 16.f;
  static constexpr float SpaceXL = 24.f;
  static constexpr float PadPanel = 12.f;
  static constexpr float RowH = 28.f;
  static constexpr float TopBarH = 44.f;
  static constexpr float StatusBarH = 26.f;
  static constexpr float NavCollapsed = 52.f;
  static constexpr float NavExpanded = 148.f;
  static constexpr float Splitter = 5.f;
  static constexpr float SectionHeaderH = 28.f;
  static constexpr float BodyFontPx = 14.5f;
  static constexpr float LargeFontPx = 18.f;
  static constexpr float HeroFontPx = 22.f;
};

/** Current UI scale (1.0 = 96 DPI / 1× content scale). */
float Scale();

/** Scale a logical pixel value by the current DPI factor. */
float S(float logical_px);

ImVec2 S2(float x, float y);

/** Sets ImGuiStyle colors, rounding, and spacing. Call once after
 * ImGui::CreateContext(), and again after a DPI change (fonts must be
 * rebuilt separately via LoadFonts). */
void Apply();

/** Loads Roboto-Medium at body/large/hero sizes × Scale(). Clears and
 * rebuilds the font atlas. Falls back to the default ImGui font. */
void LoadFonts(ImGuiIO &io);

/** Update Scale() from a GLFW monitor content-scale factor. Returns true
 * when the scale changed enough that fonts/style should be rebuilt. */
bool SetContentScale(float content_scale_x);

ImU32 U32(const ImVec4 &c);
ImU32 U32A(const ImVec4 &c, float alpha);

} // namespace fw::theme
