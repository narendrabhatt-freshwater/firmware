#include "theme.hpp"

#include <cmath>
#include <string>

namespace fw::theme
{

const Palette kPalette{
    /*bg*/ ImVec4(0.067f, 0.075f, 0.086f, 1.f),           // #111316
    /*bg_alt*/ ImVec4(0.047f, 0.055f, 0.067f, 1.f),       // #0c0e11
    /*panel*/ ImVec4(0.102f, 0.110f, 0.122f, 1.f),        // #1a1c1f
    /*panel_alt*/ ImVec4(0.118f, 0.125f, 0.137f, 1.f),    // #1e2023
    /*panel_high*/ ImVec4(0.157f, 0.165f, 0.176f, 1.f),   // #282a2d
    /*border*/ ImVec4(0.231f, 0.286f, 0.294f, 1.f),       // #3b494b
    /*text*/ ImVec4(0.886f, 0.886f, 0.902f, 1.f),         // #e2e2e6
    /*text_dim*/ ImVec4(0.725f, 0.792f, 0.796f, 1.f),     // #b9cacb
    /*accent*/ ImVec4(0.000f, 0.859f, 0.914f, 1.f),       // #00dbe9
    /*accent_bright*/ ImVec4(0.000f, 0.941f, 1.000f, 1.f),// #00f0ff
    /*accent_dim*/ ImVec4(0.000f, 0.310f, 0.329f, 1.f),   // #004f54
    /*success*/ ImVec4(0.184f, 0.973f, 0.004f, 1.f),      // #2ff801-ish
    /*warning*/ ImVec4(1.000f, 0.714f, 0.576f, 1.f),      // #ffb693
    /*danger*/ ImVec4(1.000f, 0.357f, 0.357f, 1.f),
    /*scope_trace*/ ImVec4(0.180f, 0.950f, 0.420f, 1.f),
};

Fonts g_fonts{};

namespace
{
float g_scale = 1.f;
} // namespace

float Scale() { return g_scale; }
float S(float logical_px) { return logical_px * g_scale; }
ImVec2 S2(float x, float y) { return ImVec2(S(x), S(y)); }

bool SetContentScale(float)
{
  if (std::fabs(g_scale - 1.f) < 0.001f) {
    return false;
  }
  g_scale = 1.f;
  return true;
}

ImU32 U32(const ImVec4 &c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImU32 U32A(const ImVec4 &c, float alpha)
{
  ImVec4 cc = c;
  cc.w = alpha;
  return ImGui::ColorConvertFloat4ToU32(cc);
}

void CapsLabel(const char *text, const ImVec4 &col)
{
  if (g_fonts.caps) {
    ImGui::PushFont(g_fonts.caps);
  } else if (g_fonts.mono) {
    ImGui::PushFont(g_fonts.mono);
  }
  ImGui::TextColored(col, "%s", text);
  if (g_fonts.caps || g_fonts.mono) {
    ImGui::PopFont();
  }
}

void Apply()
{
  ImGuiStyle &s = ImGui::GetStyle();
  ImVec4 *c = s.Colors;

  c[ImGuiCol_Text] = kPalette.text;
  c[ImGuiCol_TextDisabled] = kPalette.text_dim;
  c[ImGuiCol_WindowBg] = kPalette.bg;
  c[ImGuiCol_ChildBg] = kPalette.panel;
  c[ImGuiCol_PopupBg] =
      ImVec4(kPalette.panel_high.x, kPalette.panel_high.y, kPalette.panel_high.z,
             0.98f);
  c[ImGuiCol_Border] =
      ImVec4(kPalette.border.x, kPalette.border.y, kPalette.border.z, 0.75f);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = kPalette.panel_alt;
  c[ImGuiCol_FrameBgHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y, kPalette.accent_dim.z,
             0.65f);
  c[ImGuiCol_FrameBgActive] = kPalette.accent_dim;
  c[ImGuiCol_TitleBg] = kPalette.bg_alt;
  c[ImGuiCol_TitleBgActive] = kPalette.bg_alt;
  c[ImGuiCol_TitleBgCollapsed] = kPalette.bg_alt;
  c[ImGuiCol_MenuBarBg] = kPalette.panel;
  c[ImGuiCol_ScrollbarBg] = kPalette.bg_alt;
  c[ImGuiCol_ScrollbarGrab] = kPalette.panel_high;
  c[ImGuiCol_ScrollbarGrabHovered] = kPalette.accent_dim;
  c[ImGuiCol_ScrollbarGrabActive] = kPalette.accent;
  c[ImGuiCol_CheckMark] = kPalette.accent_bright;
  c[ImGuiCol_SliderGrab] = kPalette.accent;
  c[ImGuiCol_SliderGrabActive] = kPalette.accent_bright;
  c[ImGuiCol_Button] = kPalette.panel_high;
  c[ImGuiCol_ButtonHovered] = kPalette.accent_dim;
  c[ImGuiCol_ButtonActive] = kPalette.accent;
  c[ImGuiCol_Header] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y, kPalette.accent_dim.z,
             0.45f);
  c[ImGuiCol_HeaderHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y, kPalette.accent_dim.z,
             0.70f);
  c[ImGuiCol_HeaderActive] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.80f);
  c[ImGuiCol_Separator] = kPalette.border;
  c[ImGuiCol_SeparatorHovered] = kPalette.accent_dim;
  c[ImGuiCol_SeparatorActive] = kPalette.accent;
  c[ImGuiCol_ResizeGrip] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y, kPalette.accent_dim.z,
             0.30f);
  c[ImGuiCol_ResizeGripHovered] =
      ImVec4(kPalette.accent_dim.x, kPalette.accent_dim.y, kPalette.accent_dim.z,
             0.60f);
  c[ImGuiCol_ResizeGripActive] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.90f);
  c[ImGuiCol_PlotLines] = kPalette.scope_trace;
  c[ImGuiCol_PlotLinesHovered] = kPalette.accent_bright;
  c[ImGuiCol_PlotHistogram] = kPalette.warning;
  c[ImGuiCol_PlotHistogramHovered] = kPalette.danger;
  c[ImGuiCol_TableHeaderBg] = kPalette.panel_high;
  c[ImGuiCol_TableBorderStrong] = kPalette.border;
  c[ImGuiCol_TableBorderLight] =
      ImVec4(kPalette.border.x, kPalette.border.y, kPalette.border.z, 0.45f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] =
      ImVec4(kPalette.panel.x, kPalette.panel.y, kPalette.panel.z, 0.35f);
  c[ImGuiCol_TextSelectedBg] =
      ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.35f);

  s.WindowRounding = 4.f;
  s.ChildRounding = 4.f;
  s.FrameRounding = 4.f;
  s.PopupRounding = 4.f;
  s.ScrollbarRounding = 4.f;
  s.GrabRounding = 4.f;
  s.TabRounding = 4.f;
  s.WindowBorderSize = 0.f;
  s.ChildBorderSize = 1.f;
  s.PopupBorderSize = 1.f;
  s.FrameBorderSize = 1.f;
  s.WindowPadding = ImVec2(Metrics::SpaceM, Metrics::SpaceS);
  s.FramePadding = ImVec2(8.f, 5.f);
  s.ItemSpacing = ImVec2(Metrics::SpaceS, 6.f);
  s.ItemInnerSpacing = ImVec2(6.f, 4.f);
  s.IndentSpacing = 14.f;
  s.ScrollbarSize = 10.f;
  s.GrabMinSize = 10.f;
}

void LoadFonts(ImGuiIO &io)
{
  io.Fonts->Clear();
  g_fonts = {};

  ImFontConfig cfg;
  cfg.OversampleH = 3;
  cfg.OversampleV = 3;
  cfg.PixelSnapH = false;

  static ImVector<ImWchar> ranges;
  ranges.clear();
  {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    static const ImWchar extra[] = {0x2010, 0x2027, 0x2192, 0x2192,
                                    0x2260, 0x2260, 0};
    builder.AddRanges(extra);
    builder.BuildRanges(&ranges);
  }
  cfg.GlyphRanges = ranges.Data;

#ifdef FW_UI_FONT_DIR
  const std::string ui_dir = FW_UI_FONT_DIR;
  const std::string inter = ui_dir + "/Inter-Regular.ttf";
  const std::string mono = ui_dir + "/JetBrainsMono-Regular.ttf";
  g_fonts.body =
      io.Fonts->AddFontFromFileTTF(inter.c_str(), Metrics::BodyFontPx, &cfg);
  g_fonts.large =
      io.Fonts->AddFontFromFileTTF(inter.c_str(), Metrics::LargeFontPx, &cfg);
  g_fonts.hero =
      io.Fonts->AddFontFromFileTTF(inter.c_str(), Metrics::HeroFontPx, &cfg);
  g_fonts.caps =
      io.Fonts->AddFontFromFileTTF(mono.c_str(), Metrics::CapsFontPx, &cfg);
  g_fonts.mono =
      io.Fonts->AddFontFromFileTTF(mono.c_str(), Metrics::MonoFontPx, &cfg);
#endif

#ifdef FW_FONT_DIR
  if (!g_fonts.body) {
    const std::string path = std::string(FW_FONT_DIR) + "/Roboto-Medium.ttf";
    g_fonts.body =
        io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::BodyFontPx, &cfg);
    g_fonts.large =
        io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::LargeFontPx, &cfg);
    g_fonts.hero =
        io.Fonts->AddFontFromFileTTF(path.c_str(), Metrics::HeroFontPx, &cfg);
  }
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
  if (!g_fonts.mono) {
    g_fonts.mono = g_fonts.body;
  }
  if (!g_fonts.caps) {
    g_fonts.caps = g_fonts.mono;
  }
  io.FontDefault = g_fonts.body;
}

} // namespace fw::theme
