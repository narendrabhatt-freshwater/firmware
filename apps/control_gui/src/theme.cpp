#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace fw::theme
{

const Palette kPalette{
    /*bg*/ ImVec4(0.055f, 0.063f, 0.078f, 1.00f),
    /*bg_alt*/ ImVec4(0.075f, 0.086f, 0.106f, 1.00f),
    /*panel*/ ImVec4(0.100f, 0.114f, 0.140f, 1.00f),
    /*panel_alt*/ ImVec4(0.135f, 0.152f, 0.185f, 1.00f),
    /*border*/ ImVec4(0.230f, 0.260f, 0.310f, 1.00f),
    /*text*/ ImVec4(0.920f, 0.940f, 0.960f, 1.00f),
    /*text_dim*/ ImVec4(0.550f, 0.600f, 0.660f, 1.00f),
    /*accent*/ ImVec4(0.204f, 0.850f, 0.750f, 1.00f),
    /*accent_dim*/ ImVec4(0.145f, 0.430f, 0.400f, 1.00f),
    /*success*/ ImVec4(0.300f, 0.850f, 0.450f, 1.00f),
    /*warning*/ ImVec4(0.950f, 0.700f, 0.250f, 1.00f),
    /*danger*/ ImVec4(0.950f, 0.350f, 0.350f, 1.00f),
};

Fonts g_fonts{};

namespace
{
float g_scale = 1.f;
} // namespace

float Scale() { return g_scale; }

float S(float logical_px) { return logical_px * g_scale; }

ImVec2 S2(float x, float y) { return ImVec2(S(x), S(y)); }

bool SetContentScale(float content_scale_x)
{
  const float next = std::clamp(content_scale_x, 0.75f, 3.f);
  if (std::fabs(next - g_scale) < 0.04f) {
    return false;
  }
  g_scale = next;
  return true;
}

ImU32 U32(const ImVec4 &c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImU32 U32A(const ImVec4 &c, float alpha)
{
  ImVec4 cc = c;
  cc.w = alpha;
  return ImGui::ColorConvertFloat4ToU32(cc);
}

void Apply()
{
  ImGuiStyle &s = ImGui::GetStyle();
  ImVec4 *c = s.Colors;

  c[ImGuiCol_Text] = kPalette.text;
  c[ImGuiCol_TextDisabled] = kPalette.text_dim;
  c[ImGuiCol_WindowBg] = kPalette.bg;
  c[ImGuiCol_ChildBg] = kPalette.panel;
  c[ImGuiCol_PopupBg] = ImVec4(kPalette.panel_alt.x, kPalette.panel_alt.y,
                               kPalette.panel_alt.z, 0.98f);
  c[ImGuiCol_Border] = ImVec4(kPalette.border.x, kPalette.border.y,
                              kPalette.border.z, 0.60f);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = kPalette.panel_alt;
  c[ImGuiCol_FrameBgHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y,
             kPalette.accent_dim.z, 0.55f);
  c[ImGuiCol_FrameBgActive] = kPalette.accent_dim;
  c[ImGuiCol_TitleBg] = kPalette.bg;
  c[ImGuiCol_TitleBgActive] = kPalette.bg;
  c[ImGuiCol_TitleBgCollapsed] = kPalette.bg;
  c[ImGuiCol_MenuBarBg] = kPalette.panel;
  c[ImGuiCol_ScrollbarBg] = kPalette.bg;
  c[ImGuiCol_ScrollbarGrab] = kPalette.panel_alt;
  c[ImGuiCol_ScrollbarGrabHovered] = kPalette.accent_dim;
  c[ImGuiCol_ScrollbarGrabActive] = kPalette.accent;
  c[ImGuiCol_CheckMark] = kPalette.accent;
  c[ImGuiCol_SliderGrab] = kPalette.accent_dim;
  c[ImGuiCol_SliderGrabActive] = kPalette.accent;
  c[ImGuiCol_Button] = kPalette.panel_alt;
  c[ImGuiCol_ButtonHovered] = kPalette.accent_dim;
  c[ImGuiCol_ButtonActive] = kPalette.accent;
  c[ImGuiCol_Header] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y,
             kPalette.accent_dim.z, 0.35f);
  c[ImGuiCol_HeaderHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y,
             kPalette.accent_dim.z, 0.60f);
  c[ImGuiCol_HeaderActive] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.70f);
  c[ImGuiCol_Separator] = kPalette.border;
  c[ImGuiCol_SeparatorHovered] = kPalette.accent_dim;
  c[ImGuiCol_SeparatorActive] = kPalette.accent;
  c[ImGuiCol_ResizeGrip] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y,
             kPalette.accent_dim.z, 0.30f);
  c[ImGuiCol_ResizeGripHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y,
             kPalette.accent_dim.z, 0.60f);
  c[ImGuiCol_ResizeGripActive] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.90f);
  c[ImGuiCol_PlotLines] = kPalette.accent;
  c[ImGuiCol_PlotLinesHovered] = kPalette.warning;
  c[ImGuiCol_PlotHistogram] = kPalette.warning;
  c[ImGuiCol_PlotHistogramHovered] = kPalette.danger;
  c[ImGuiCol_TableHeaderBg] = kPalette.panel_alt;
  c[ImGuiCol_TableBorderStrong] = kPalette.border;
  c[ImGuiCol_TableBorderLight] =
      ImVec4(kPalette.border.x, kPalette.border.y, kPalette.border.z, 0.50f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] =
      ImVec4(kPalette.panel.x, kPalette.panel.y, kPalette.panel.z, 0.35f);
  c[ImGuiCol_TextSelectedBg] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.35f);

  const float sc = g_scale;
  s.WindowRounding = 8.f * sc;
  s.ChildRounding = 8.f * sc;
  s.FrameRounding = 5.f * sc;
  s.PopupRounding = 6.f * sc;
  s.ScrollbarRounding = 6.f * sc;
  s.GrabRounding = 5.f * sc;
  s.TabRounding = 5.f * sc;
  s.WindowBorderSize = 1.f;
  s.ChildBorderSize = 1.f;
  s.PopupBorderSize = 1.f;
  s.FrameBorderSize = 1.f;
  s.WindowPadding = ImVec2(Metrics::SpaceM * sc, Metrics::SpaceS * sc);
  s.FramePadding = ImVec2(7.f * sc, 4.f * sc);
  s.ItemSpacing = ImVec2(Metrics::SpaceS * sc, 5.f * sc);
  s.ItemInnerSpacing = ImVec2(6.f * sc, 4.f * sc);
  s.IndentSpacing = 14.f * sc;
  s.ScrollbarSize = 12.f * sc;
  s.GrabMinSize = 8.f * sc;
}

void LoadFonts(ImGuiIO &io)
{
  io.Fonts->Clear();
  g_fonts = {};

  ImFontConfig cfg;
  cfg.OversampleH = 3;
  cfg.OversampleV = 3;
  cfg.PixelSnapH = false;

  // Default glyph ranges stop at Latin-1, but the UI's copy uses General
  // Punctuation (en/em dash, arrows, "not equal") — extend the range or
  // those render as tofu '?' glyphs.
  static ImVector<ImWchar> ranges;
  ranges.clear();
  {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    static const ImWchar extra[] = {
        0x2010, 0x2027, // hyphen, en/em dash, quotes, bullet, ellipsis, …
        0x2192, 0x2192, // →
        0x2260, 0x2260, // ≠
        0,
    };
    builder.AddRanges(extra);
    builder.BuildRanges(&ranges);
  }
  cfg.GlyphRanges = ranges.Data;

  const float sc = g_scale;
#ifdef FW_FONT_DIR
  const std::string path = std::string(FW_FONT_DIR) + "/Roboto-Medium.ttf";
  g_fonts.body =
      io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::BodyFontPx * sc, &cfg);
  g_fonts.large =
      io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::LargeFontPx * sc, &cfg);
  g_fonts.hero =
      io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::HeroFontPx * sc, &cfg);
#endif

  if (!g_fonts.body) {
    g_fonts.body = io.Fonts->AddFontDefault();
  }
  if (!g_fonts.large) {
    g_fonts.large = g_fonts.body;
  }
  if (!g_fonts.hero) {
    g_fonts.hero = g_fonts.large;
  }
  io.FontDefault = g_fonts.body;
}

} // namespace fw::theme
