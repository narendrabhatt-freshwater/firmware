#pragma once

#include "imgui.h"

/** Custom-drawn widgets for the Freshwater Control phosphor design. All are
 * self-contained (own eased state keyed by ImGuiID) so callers just pass
 * current values each frame, immediate-mode style. */
namespace fw::ui
{

/** Design button variants (mirrors the React design's inline styles). */
enum class BtnKind : int
{
  Primary = 0, // green tint bg, phosphor text
  Neutral,     // raised bg, dim text
  Danger,      // red tint bg, fault text
  Warn,        // amber tint bg, warn text
};

/** Rounded-2 mono button. size.x < 0 → fill available width. */
bool Btn(const char *label, const ImVec2 &size = ImVec2(0, 0),
         BtnKind kind = BtnKind::Neutral);

/** Tiny chip button (9 px mono, 2 px padding) for quick-sends / voice picks.
 * `selected` renders the phosphor-selected style. */
bool ChipBtn(const char *label, bool selected = false,
             BtnKind kind = BtnKind::Neutral);

/** Compatibility wrapper: danger → Danger, otherwise Primary. */
bool GlowButton(const char *label, const ImVec2 &size = ImVec2(0, 0),
                bool danger = false);

/** 32×16 sliding toggle switch (design Effect page). Returns true on change. */
bool ToggleSwitch(const char *str_id, bool *value, bool enabled = true);

/** Toggle row: switch + mono label + ON/OFF readout. */
bool ToggleRow(const char *label, bool *value, bool enabled = true);

/** Small status dot (drawn inline, advances cursor). glow adds a soft halo. */
void StatusDot(float radius, const ImVec4 &color, bool glow = false);

/** Small rounded connection indicator: dot + label. */
void StatusPill(const char *str_id, const char *label, bool active,
                bool alert = false);

/** Design panel: bordered child, panel bg, mono 9px letterspaced title row
 * with bottom border. Pair with EndSection(). Cursor stays on the title row
 * so callers can SameLine() header actions, then NewLine() before the body. */
bool BeginSection(const char *str_id, const char *title,
                  const ImVec2 &size = ImVec2(0, 0),
                  bool border = true);

void EndSection();

/** Empty-state placeholder centered in the remaining region. */
void EmptyState(const char *title, const char *detail);

/** Phosphor scope: #070908 bed, 10×8 graticule with brighter centre axes and
 * minor ticks, multi-pass glow trace. */
void GlowWaveform(const char *str_id, const float *samples, int count,
                  const ImVec2 &size, float scale_min = -1.f,
                  float scale_max = 1.f);

/** Horizontal level meter with fast attack / slow release easing. */
void LevelMeter(const char *str_id, float value01, const ImVec2 &size,
                float peak_hold01 = -1.f);

/** Vertical LED-style level meter (design LVL bar). */
void VerticalMeter(const char *str_id, float value01, const ImVec2 &size,
                   float peak_hold01 = -1.f);

/** Thin phosphor progress bar (value 0..1). */
void ProgressBar(const char *str_id, float value01, const ImVec2 &size);

/** Drag grip between panels. `horizontal_bar` true → resize north/south. */
float Splitter(const char *str_id, bool horizontal_bar, float thickness = 0.f,
               float cross_axis_size = 0.f);

/** Nav rail icons (drawn vectors — no font glyph dependency). */
enum class NavIcon : int
{
  Perform = 0, // ◈ filled diamond in outline
  Tone,        // ◇ outline diamond
  Sample,      // ⊟ boxed minus
  Effect,      // ⊞ boxed plus
  Setup,       // ◧ half-filled square
};

void DrawNavIcon(ImDrawList *dl, NavIcon icon, ImVec2 center, float half,
                 ImU32 col);

/** Small warning triangle + '!' (design ⚠ substitute). Advances cursor. */
void WarnIcon(const ImVec4 &col, float size = 11.f);

/** Small square button with a drawn circular-refresh icon (design ↻, which
 * the shipped fonts lack). size <= 0 → scaled 26×22 default. */
bool RefreshBtn(const char *str_id, const ImVec2 &size = ImVec2(0.f, 0.f));

} // namespace fw::ui
