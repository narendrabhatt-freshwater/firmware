#include "toast.hpp"

#include "theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace fw::ui
{

void ToastHost::Push(ToastKind kind, std::string text, float life_sec)
{
  if (text.empty()) {
    return;
  }
  Toast t;
  t.kind = kind;
  t.text = std::move(text);
  t.life = life_sec;
  items_.push_back(std::move(t));
  while (items_.size() > 6) {
    items_.erase(items_.begin());
  }
}

void ToastHost::Tick(float dt)
{
  for (auto &t : items_) {
    t.life -= dt;
  }
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [](const Toast &t) { return t.life <= 0.f; }),
               items_.end());
}

void ToastHost::Draw()
{
  if (items_.empty()) {
    return;
  }
  using fw::theme::kPalette;
  using fw::theme::S;
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float pad = S(fw::theme::Metrics::SpaceM);
  const float width = S(320.f);
  float y = vp->WorkPos.y + pad + S(fw::theme::Metrics::TopBarH);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(8.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12.f), S(10.f)));
  for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
    const Toast &t = items_[static_cast<std::size_t>(i)];
    ImVec4 bg = kPalette.panel_alt;
    ImVec4 accent = kPalette.accent;
    if (t.kind == ToastKind::Error) {
      accent = kPalette.danger;
      bg = ImVec4(0.22f, 0.08f, 0.08f, 0.96f);
    } else if (t.kind == ToastKind::Warning) {
      accent = kPalette.warning;
      bg = ImVec4(0.22f, 0.16f, 0.06f, 0.96f);
    } else if (t.kind == ToastKind::Success) {
      accent = kPalette.success;
    }
    const float alpha = std::clamp(t.life / 0.35f, 0.f, 1.f);
    bg.w *= alpha;
    accent.w *= alpha;

    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - width - pad, y),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    char id[32];
    std::snprintf(id, sizeof(id), "##toast%d", i);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    const char *tag = (t.kind == ToastKind::Error)     ? "ERR"
                      : (t.kind == ToastKind::Warning) ? "WARN"
                      : (t.kind == ToastKind::Success) ? "OK"
                                                       : "INFO";
    ImGui::TextUnformatted(tag);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::TextWrapped("%s", t.text.c_str());
    ImGui::PopStyleVar();
    y += ImGui::GetWindowSize().y + S(6.f);
    ImGui::End();
    ImGui::PopStyleColor(2);
  }
  ImGui::PopStyleVar(2);
}

} // namespace fw::ui
