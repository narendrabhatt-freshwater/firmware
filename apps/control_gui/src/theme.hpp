#pragma once

#include "imgui.h"

/** CMI Control visual identity — aligned to the Stitch "Modern
 * Industrial Console" design system (dark void, cyber cyan, neon green). */
namespace fw::theme
{

struct Palette
{
  ImVec4 bg;           // #111316 canvas
  ImVec4 bg_alt;       // #0c0e11 deepest
  ImVec4 panel;        // #1a1c1f / surface-container-low
  ImVec4 panel_alt;    // #1e2023 / surface-container
  ImVec4 panel_high;   // #282a2d
  ImVec4 border;       // #3b494b outline-variant
  ImVec4 text;         // #e2e2e6
  ImVec4 text_dim;     // #b9cacb
  ImVec4 accent;       // #00dbe9 cyber cyan
  ImVec4 accent_bright;// #00f0ff primary container
  ImVec4 accent_dim;   // #004f54
  ImVec4 success;      // neon green online
  ImVec4 warning;      // safety orange
  ImVec4 danger;       // silence / fault red
  ImVec4 scope_trace;  // waveform green
};

extern const Palette kPalette;

struct Fonts
{
  ImFont *body = nullptr;   // Inter / Roboto UI
  ImFont *large = nullptr;  // headers
  ImFont *hero = nullptr;   // hero readouts
  ImFont *mono = nullptr;   // JetBrains Mono data / log
  ImFont *caps = nullptr;   // small caps labels
};

extern Fonts g_fonts;

struct Metrics
{
  static constexpr float SpaceXS = 4.f;
  static constexpr float SpaceS = 8.f;
  static constexpr float SpaceM = 12.f;
  static constexpr float SpaceL = 16.f;
  static constexpr float SpaceXL = 24.f;
  static constexpr float TopNavH = 52.f;
  static constexpr float TopBarH = 52.f; // alias
  static constexpr float FooterH = 48.f;
  static constexpr float StatusBarH = 48.f; // alias
  static constexpr float LogW = 280.f;
  static constexpr float TelemetryH = 56.f;
  static constexpr float RowH = 28.f;
  static constexpr float NavCollapsed = 52.f;
  static constexpr float NavExpanded = 148.f;
  static constexpr float Splitter = 5.f;
  static constexpr float BodyFontPx = 14.f;
  static constexpr float LargeFontPx = 18.f;
  static constexpr float HeroFontPx = 22.f;
  static constexpr float MonoFontPx = 13.f;
  static constexpr float CapsFontPx = 11.f;
};

float Scale();
float S(float logical_px);
ImVec2 S2(float x, float y);

void Apply();
void LoadFonts(ImGuiIO &io);
bool SetContentScale(float content_scale_x);

ImU32 U32(const ImVec4 &c);
ImU32 U32A(const ImVec4 &c, float alpha);

/** All-caps label in mono/caps font. */
void CapsLabel(const char *text, const ImVec4 &col = kPalette.text_dim);

} // namespace fw::theme
