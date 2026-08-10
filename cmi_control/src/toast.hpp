#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fw::ui
{

enum class ToastKind : uint8_t
{
  Info = 0,
  Success,
  Warning,
  Error,
};

struct Toast
{
  ToastKind kind = ToastKind::Info;
  std::string text;
  float life = 3.5f;
};

/** Transient notification stack (top-right of the main column). */
class ToastHost
{
public:
  void Push(ToastKind kind, std::string text, float life_sec = 3.5f);
  void Tick(float dt);
  void Draw();

private:
  std::vector<Toast> items_;
};

} // namespace fw::ui
