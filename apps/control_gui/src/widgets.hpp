#pragma once

#include "imgui.h"

/** Animated, custom-drawn widgets for the Freshwater Control theme. All are
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

/** Ivory piano key: dips and tints toward the accent color on press, and
 * glows softly while `sounding` (e.g. the note is active elsewhere). Caller
 * reads ImGui::IsItemActivated()/IsItemDeactivated() right after the call,
 * same convention as a plain ImGui::Button held for note-on/note-off. */
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
 * color ramp (accent → warning → danger) as it approaches full scale. */
void LevelMeter(const char *str_id, float value01, const ImVec2 &size);

/** Neon-style multi-pass glow waveform with a light scope graticule. */
void GlowWaveform(const char *str_id, const float *samples, int count,
                  const ImVec2 &size, float scale_min = -1.f,
                  float scale_max = 1.f);

/** Custom-drawn button with an eased hover/press glow. */
bool GlowButton(const char *label, const ImVec2 &size = ImVec2(0, 0),
                bool danger = false);

/** Compact voice-slot chip for the Perform strip; `glow01` eases in/out as
 * the voice activates/deactivates. `selected` draws a stronger border. */
void VoiceSlotBadge(const char *str_id, const char *label, bool active,
                    float glow01, const char *sub = nullptr,
                    bool selected = false);

/** Compact row of `count` hex-labelled chips (n0, n1, ...) for picking a
 * voice — click to select, replaces slider-based voice pickers. Returns
 * true if the click changed `*current` this frame. */
bool VoiceSelector(const char *str_id, int *current, int count);

/** Thin accent progress bar (value 0..1). */
void ProgressBar(const char *str_id, float value01, const ImVec2 &size);

/** Collapsible left nav rail. Returns true if a nav item was clicked.
 * `*expanded` / `*anim_width` own drawer width animation. */
bool NavDrawer(const char *str_id, const char *const *labels, int count,
               int *current, bool *expanded, float *anim_width);

/** Drag grip between panels. `horizontal_bar` true → resize north/south
 * (stacked panels); false → east/west (side-by-side). Returns mouse delta
 * along the resize axis while dragging, else 0. Caller applies delta to a
 * stored size and clamps.
 * `cross_axis_size` > 0 overrides the long-axis length (needed after
 * SameLine, where GetContentRegionAvail() is not the column height). */
float Splitter(const char *str_id, bool horizontal_bar, float thickness = 5.f,
               float cross_axis_size = 0.f);

} // namespace fw::ui
