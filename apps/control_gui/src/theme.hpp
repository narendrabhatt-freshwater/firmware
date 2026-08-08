#pragma once

#include "imgui.h"

/** Freshwater Control visual identity — phosphor-green terminal design
 * (vite/Freshwater Control App Design). Dark void greens, JetBrains Mono
 * data, Inter body. */
namespace fw::theme
{

struct Palette
{
  ImVec4 bg;            // #070908 app background
  ImVec4 bg_alt;        // #0c0f0c surface (bars, cards)
  ImVec4 panel;         // #101310 panel
  ImVec4 panel_alt;     // #161b16 raised
  ImVec4 panel_high;    // #1c231c hover / scrollbar
  ImVec4 border;        // #1a211a
  ImVec4 border_hi;     // #243024 focused / popover border
  ImVec4 text;          // #c2dcc2 fg
  ImVec4 text_dim;      // #607060 fg-dim
  ImVec4 muted;         // #334433
  ImVec4 accent;        // #4ade80 phosphor
  ImVec4 accent_bright; // #86efac phosphor-text
  ImVec4 accent_dim;    // #22c55e phosphor-dim
  ImVec4 success;       // phosphor (ok)
  ImVec4 warning;       // #fbbf24
  ImVec4 danger;        // #f87171 fault
  ImVec4 scope_trace;   // waveform phosphor
};

extern const Palette kPalette;

struct Fonts
{
  ImFont *body = nullptr;       // Inter UI, 13px
  ImFont *large = nullptr;      // headers
  ImFont *hero = nullptr;       // hero readouts
  ImFont *mono = nullptr;       // JetBrains Mono data / log, 12px
  ImFont *caps = nullptr;       // small mono labels, 10px
  ImFont *mono_small = nullptr; // tiny mono annotations, 9px
  ImFont *icons = nullptr;      // JetBrains Mono 15px for nav symbols
};

extern Fonts g_fonts;

struct Metrics
{
  static constexpr float SpaceXS = 4.f;
  static constexpr float SpaceS = 8.f;
  static constexpr float SpaceM = 12.f;
  static constexpr float SpaceL = 16.f;
  static constexpr float SpaceXL = 24.f;
  static constexpr float SidebarW = 56.f;    // left icon rail
  static constexpr float StatusBarH = 40.f;  // top chip bar
  static constexpr float FaultBannerH = 32.f;
  static constexpr float LogW = 288.f;       // right log + console panel
  static constexpr float LogRailW = 28.f;    // collapsed log rail
  static constexpr float RowH = 24.f;
  static constexpr float Splitter = 5.f;
  static constexpr float BodyFontPx = 14.f;
  static constexpr float LargeFontPx = 18.f;
  static constexpr float HeroFontPx = 22.f;
  static constexpr float MonoFontPx = 13.f;
  static constexpr float CapsFontPx = 11.f;
  static constexpr float MonoSmallFontPx = 10.f;
  static constexpr float IconFontPx = 16.f;
};

/** Effective UI scale: user zoom × platform content scale. All layout code
 * expresses sizes in logical px through S()/S2(); fonts are baked at the
 * scaled size so text stays crisp at any zoom. */
float Scale();
float S(float logical_px);
ImVec2 S2(float x, float y);

/** User zoom factor (persisted; Cmd/Ctrl +/-/0). Clamped 0.75–1.75.
 * Marks fonts dirty when it changes. */
void SetUserZoom(float zoom);
float UserZoom();

/** Platform content scale (Linux/Windows HiDPI). macOS stays 1.0 — its
 * point coordinate system already handles Retina via framebuffer scale. */
void SetContentScale(float content_scale_x);

/** True once after the effective scale changed; caller rebuilds the font
 * atlas + backend texture and re-applies the style outside a frame. */
bool ConsumeFontsDirty();

void Apply();
void LoadFonts(ImGuiIO &io);

ImU32 U32(const ImVec4 &c);
ImU32 U32A(const ImVec4 &c, float alpha);

/** 0.3..1.0 opacity square wave-ish blink matching the design's 1.1 s CSS
 * animation (ease in/out). */
float Blink01();

/** All-caps label in mono/caps font. */
void CapsLabel(const char *text, const ImVec4 &col = kPalette.text_dim);

} // namespace fw::theme
