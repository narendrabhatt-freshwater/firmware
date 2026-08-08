#pragma once

#include "imgui.h"

/** Animated, custom-drawn widgets for the CMI Control theme. All are
 * self-contained (own eased state keyed by ImGuiID) so callers just pass
 * current values each frame, immediate-mode style. */
namespace fw::ui
{

/** Small rounded connection indicator: dot + label. Pulses gently while
 * `active`, pulses fast in the danger color while `alert`. */
void StatusPill(const char *str_id, const char *label, bool active,
                bool alert = false);

/** Card/section title, with an optional right-aligned StatusPill on the same
 * line (pass pill_id = nullptr to omit it). */
void SectionHeader(const char *title, const char *pill_id = nullptr,
                   const char *pill_label = nullptr, bool active = false,
                   bool alert = false);

/** Shared section card: bordered child with padded title row. Pair with
 * EndSection(). Optional `header_right` draws after the title (same line). */
bool BeginSection(const char *str_id, const char *title,
                  const ImVec2 &size = ImVec2(0, 0),
                  bool border = true);

void EndSection();

/** Empty-state placeholder centered in the remaining region. */
void EmptyState(const char *title, const char *detail);

/** Ivory piano key: dips and tints toward the accent color on press, and
 * glows softly while `sounding`. Caller reads IsItemActivated()/Deactivated. */
void PianoKey(const char *str_id, const char *label, const ImVec2 &size,
             bool sounding = false);

/** Compact segmented control with a sliding highlight pill. */
void SegmentedControl(const char *str_id, const char *const *labels,
                      int count, int *current);

/** View switcher styled like SegmentedControl but taller, for top-level
 * navigation. */
void AnimatedTabBar(const char *str_id, const char *const *labels, int count,
                    int *current);

/** Horizontal level meter with fast attack / slow release easing and a
 * color ramp (accent → warning → danger). Optional peak-hold needle. */
void LevelMeter(const char *str_id, float value01, const ImVec2 &size,
                float peak_hold01 = -1.f);

/** Neon-style multi-pass glow waveform with a light scope graticule. */
void GlowWaveform(const char *str_id, const float *samples, int count,
                  const ImVec2 &size, float scale_min = -1.f,
                  float scale_max = 1.f);

/** Custom-drawn button with an eased hover/press glow. */
bool GlowButton(const char *label, const ImVec2 &size = ImVec2(0, 0),
                bool danger = false);

/** Compact voice-slot chip for the Perform strip. */
void VoiceSlotBadge(const char *str_id, const char *label, bool active,
                    float glow01, const char *sub = nullptr,
                    bool selected = false);

/** Compact row of `count` hex-labelled chips (n0, n1, ...) for picking a
 * voice. Returns true if the click changed `*current` this frame. */
bool VoiceSelector(const char *str_id, int *current, int count);

/** Thin accent progress bar (value 0..1). */
void ProgressBar(const char *str_id, float value01, const ImVec2 &size);

/** Collapsible left nav rail. Returns true if a nav item was clicked. */
bool NavDrawer(const char *str_id, const char *const *labels, int count,
               int *current, bool *expanded, float *anim_width);

/** Top horizontal module tabs (Stitch shell). Returns true if selection changed. */
bool TopTabs(const char *str_id, const char *const *labels, int count,
             int *current);

/** Drag grip between panels. `horizontal_bar` true → resize north/south. */
float Splitter(const char *str_id, bool horizontal_bar, float thickness = 0.f,
               float cross_axis_size = 0.f);

/** Toggle row: label left, GlowButton on/off cluster right. */
bool ToggleRow(const char *label, bool *value, bool enabled = true);

/** Rotary knob for scope TIME/DIV / VOLT/DIV. Drag vertically to change. */
bool RotaryKnob(const char *str_id, const char *label, float *value,
                float v_min, float v_max, const char *format,
                float size = 72.f);

/**
 * Stepped rotary (classic scope TIME/DIV style). Drag or scroll wheel moves
 * one detent at a time through [0, count). value_label is drawn under the dial.
 */
bool RotaryKnobStepped(const char *str_id, const char *label, int *index,
                       int count, const char *value_label, float size = 72.f);

/** Vertical LED-style level meter. */
void VerticalMeter(const char *str_id, float value01, const ImVec2 &size,
                   float peak_hold01 = -1.f);

} // namespace fw::ui
