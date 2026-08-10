#pragma once

#include <cmath>

/** Small time-based easing helpers for immediate-mode widget animation.
 * Widgets keep their own eased state (usually a static map keyed by
 * ImGuiID) and call these each frame with ImGui::GetIO().DeltaTime. */
namespace fw::anim
{

inline float ExpApproach(float current, float target, float dt, float speed)
{
  if (dt <= 0.f) {
    return current;
  }
  const float t = 1.f - std::exp(-speed * dt);
  return current + (target - current) * t;
}

/** 0..1 breathing pulse, e.g. for a "live" connection glow. */
inline float Pulse01(double time_sec, float period_sec = 1.4f)
{
  const float phase =
      std::fmod(static_cast<float>(time_sec), period_sec) / period_sec;
  return 0.5f + 0.5f * std::sin(phase * 6.2831853f);
}

} // namespace fw::anim
