#pragma once

#include "imgui.h"

/** Freshwater Control visual identity: dark/aqua palette, rounded modern
 * style, and the Roboto fonts bundled with the vendored Dear ImGui source. */
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
};

extern Fonts g_fonts;

/** Sets ImGuiStyle colors, rounding, and spacing. Call once after
 * ImGui::CreateContext(). */
void Apply();

/** Loads Roboto-Medium at UI and header sizes. Call once before the first
 * ImGui::NewFrame(). Falls back to the default ImGui font if unavailable. */
void LoadFonts(ImGuiIO &io);

ImU32 U32(const ImVec4 &c);
ImU32 U32A(const ImVec4 &c, float alpha);

} // namespace fw::theme
