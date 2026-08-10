#include "theme.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace fw::theme
{

  const Palette kPalette{
      /*bg*/ ImVec4(0.027f, 0.035f, 0.031f, 1.f),            // #070908
      /*bg_alt*/ ImVec4(0.047f, 0.059f, 0.047f, 1.f),        // #0c0f0c
      /*panel*/ ImVec4(0.063f, 0.075f, 0.063f, 1.f),         // #101310
      /*panel_alt*/ ImVec4(0.086f, 0.106f, 0.086f, 1.f),     // #161b16
      /*panel_high*/ ImVec4(0.110f, 0.137f, 0.110f, 1.f),    // #1c231c
      /*border*/ ImVec4(0.102f, 0.129f, 0.102f, 1.f),        // #1a211a
      /*border_hi*/ ImVec4(0.141f, 0.188f, 0.141f, 1.f),     // #243024
      /*text*/ ImVec4(0.761f, 0.863f, 0.761f, 1.f),          // #c2dcc2
      /*text_dim*/ ImVec4(0.376f, 0.439f, 0.376f, 1.f),      // #607060
      /*muted*/ ImVec4(0.200f, 0.267f, 0.200f, 1.f),         // #334433
      /*accent*/ ImVec4(0.290f, 0.871f, 0.502f, 1.f),        // #4ade80
      /*accent_bright*/ ImVec4(0.525f, 0.937f, 0.675f, 1.f), // #86efac
      /*accent_dim*/ ImVec4(0.133f, 0.773f, 0.369f, 1.f),    // #22c55e
      /*success*/ ImVec4(0.290f, 0.871f, 0.502f, 1.f),       // #4ade80
      /*warning*/ ImVec4(0.984f, 0.749f, 0.141f, 1.f),       // #fbbf24
      /*danger*/ ImVec4(0.973f, 0.443f, 0.443f, 1.f),        // #f87171
      /*scope_trace*/ ImVec4(0.290f, 0.871f, 0.502f, 1.f),   // #4ade80
  };

  Fonts g_fonts{};

  namespace
  {
    float g_user_zoom = 1.f;
    float g_content_scale = 1.f;
    float g_scale = 1.f;
    bool g_fonts_dirty = false;

    void RecomputeScale()
    {
      const float s = g_user_zoom * g_content_scale;
      if (std::fabs(s - g_scale) > 0.001f)
      {
        g_scale = s;
        g_fonts_dirty = true;
      }
    }
  } // namespace

  float Scale() { return g_scale; }
  float S(float logical_px) { return logical_px * g_scale; }
  ImVec2 S2(float x, float y) { return ImVec2(S(x), S(y)); }

  void SetUserZoom(float zoom)
  {
    g_user_zoom = std::fmin(std::fmax(zoom, 0.75f), 1.75f);
    RecomputeScale();
  }

  float UserZoom() { return g_user_zoom; }

  void SetContentScale(float content_scale_x)
  {
#if defined(__APPLE__)
    // macOS window coordinates are points; Retina density is handled by the
    // framebuffer scale. Multiplying here double-scales the layout.
    (void)content_scale_x;
#else
    if (content_scale_x > 0.5f)
    {
      g_content_scale = content_scale_x;
      RecomputeScale();
    }
#endif
  }

  bool ConsumeFontsDirty()
  {
    const bool dirty = g_fonts_dirty;
    g_fonts_dirty = false;
    return dirty;
  }

  ImU32 U32(const ImVec4 &c) { return ImGui::ColorConvertFloat4ToU32(c); }
  ImU32 U32A(const ImVec4 &c, float alpha)
  {
    ImVec4 cc = c;
    cc.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(cc);
  }

  float Blink01()
  {
    // CSS: opacity 1 -> 0.3 -> 1 over 1.1 s, ease-in-out ≈ cosine.
    const float t =
        std::fmod(static_cast<float>(ImGui::GetTime()), 1.1f) / 1.1f;
    return 0.65f + 0.35f * std::cos(t * 6.2831853f);
  }

  void CapsLabel(const char *text, const ImVec4 &col)
  {
    if (g_fonts.caps)
    {
      ImGui::PushFont(g_fonts.caps);
    }
    else if (g_fonts.mono)
    {
      ImGui::PushFont(g_fonts.mono);
    }
    ImGui::TextColored(col, "%s", text);
    if (g_fonts.caps || g_fonts.mono)
    {
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
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] =
        ImVec4(kPalette.panel.x, kPalette.panel.y, kPalette.panel.z, 0.99f);
    c[ImGuiCol_Border] = kPalette.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kPalette.bg_alt;
    c[ImGuiCol_FrameBgHovered] = kPalette.panel_alt;
    c[ImGuiCol_FrameBgActive] = kPalette.panel_alt;
    c[ImGuiCol_TitleBg] = kPalette.bg_alt;
    c[ImGuiCol_TitleBgActive] = kPalette.bg_alt;
    c[ImGuiCol_TitleBgCollapsed] = kPalette.bg_alt;
    c[ImGuiCol_MenuBarBg] = kPalette.panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kPalette.border_hi;
    c[ImGuiCol_ScrollbarGrabHovered] = kPalette.muted;
    c[ImGuiCol_ScrollbarGrabActive] = kPalette.accent_dim;
    c[ImGuiCol_CheckMark] = kPalette.accent;
    c[ImGuiCol_SliderGrab] = kPalette.accent;
    c[ImGuiCol_SliderGrabActive] = kPalette.accent_bright;
    c[ImGuiCol_Button] = kPalette.panel_alt;
    c[ImGuiCol_ButtonHovered] = kPalette.panel_high;
    c[ImGuiCol_ButtonActive] = kPalette.panel_high;
    c[ImGuiCol_Header] =
        ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.12f);
    c[ImGuiCol_HeaderHovered] =
        ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.18f);
    c[ImGuiCol_HeaderActive] =
        ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.25f);
    c[ImGuiCol_Separator] = kPalette.border;
    c[ImGuiCol_SeparatorHovered] = kPalette.border_hi;
    c[ImGuiCol_SeparatorActive] = kPalette.accent_dim;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = kPalette.border_hi;
    c[ImGuiCol_ResizeGripActive] = kPalette.accent_dim;
    c[ImGuiCol_PlotLines] = kPalette.scope_trace;
    c[ImGuiCol_PlotLinesHovered] = kPalette.accent_bright;
    c[ImGuiCol_PlotHistogram] = kPalette.warning;
    c[ImGuiCol_PlotHistogramHovered] = kPalette.danger;
    c[ImGuiCol_TableHeaderBg] = kPalette.bg_alt;
    c[ImGuiCol_TableBorderStrong] = kPalette.border;
    c[ImGuiCol_TableBorderLight] = kPalette.border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] =
        ImVec4(kPalette.panel.x, kPalette.panel.y, kPalette.panel.z, 0.35f);
    c[ImGuiCol_TextSelectedBg] =
        ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.30f);
    c[ImGuiCol_NavHighlight] =
        ImVec4(kPalette.accent.x, kPalette.accent.y, kPalette.accent.z, 0.60f);

    s.WindowRounding = 0.f;
    s.ChildRounding = S(2.f);
    s.FrameRounding = S(2.f);
    s.PopupRounding = S(2.f);
    s.ScrollbarRounding = S(2.f);
    s.GrabRounding = S(5.f);
    s.TabRounding = S(2.f);
    s.WindowBorderSize = 0.f;
    s.ChildBorderSize = 1.f;
    s.PopupBorderSize = 1.f;
    s.FrameBorderSize = 1.f;
    s.WindowPadding = S2(Metrics::SpaceM, Metrics::SpaceS);
    s.FramePadding = S2(7.f, 3.f);
    s.ItemSpacing = S2(Metrics::SpaceS, 6.f);
    s.ItemInnerSpacing = S2(6.f, 4.f);
    s.IndentSpacing = S(14.f);
    s.ScrollbarSize = S(6.f);
    s.GrabMinSize = S(10.f);
  }

  namespace
  {

    /* ImGui's stb_truetype atlas path hard-faults on non-font bytes (e.g. an HTML
     * download error saved as .ttf). Reject anything that is not sfnt/OTTO. */
    bool FileLooksLikeFont(const char *path)
    {
      FILE *f = std::fopen(path, "rb");
      if (!f)
      {
        return false;
      }
      unsigned char magic[4] = {};
      const size_t n = std::fread(magic, 1, 4, f);
      std::fclose(f);
      if (n < 4)
      {
        return false;
      }
      const bool ttf = magic[0] == 0x00 && magic[1] == 0x01 && magic[2] == 0x00 &&
                       magic[3] == 0x00;
      const bool otf = magic[0] == 'O' && magic[1] == 'T' && magic[2] == 'T' &&
                       magic[3] == 'O';
      const bool ttc = magic[0] == 't' && magic[1] == 't' && magic[2] == 'c' &&
                       magic[3] == 'f';
      return ttf || otf || ttc;
    }

    ImFont *AddFontIfValid(ImFontAtlas *atlas, const std::string &path, float size,
                           const ImFontConfig *cfg)
    {
      if (!FileLooksLikeFont(path.c_str()))
      {
        return nullptr;
      }
      return atlas->AddFontFromFileTTF(path.c_str(), size, cfg);
    }

  } // namespace

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
      // Arrows, geometric shapes, and misc glyphs used by the design
      // (▾ ■ ▶ ← → ↑ ↓ ↻ · …). Missing glyphs fall back per-font.
      static const ImWchar extra[] = {
          0x2010, 0x2027, // hyphens, bullets, ellipsis
          0x2190, 0x21FF, // arrows
          0x229E, 0x229F, // ⊞ ⊟
          0x23CE, 0x23CE, // ⏎
          0x2260, 0x2260,
          0x25A0, 0x25FF, // geometric shapes ◈ ◇ ◧ ▾ ■ ▶ ◁ ▷
          0x26A0, 0x26A0, // ⚠
          0x2715, 0x2715, // ✕
          0};
      builder.AddRanges(extra);
      builder.BuildRanges(&ranges);
    }
    cfg.GlyphRanges = ranges.Data;

#ifdef FW_UI_FONT_DIR
    const std::string ui_dir = FW_UI_FONT_DIR;
    const std::string inter = ui_dir + "/Inter-Regular.ttf";
    const std::string mono = ui_dir + "/JetBrainsMono-Regular.ttf";
    g_fonts.body =
        AddFontIfValid(io.Fonts, inter, S(Metrics::BodyFontPx), &cfg);
    g_fonts.large =
        AddFontIfValid(io.Fonts, inter, S(Metrics::LargeFontPx), &cfg);
    g_fonts.hero =
        AddFontIfValid(io.Fonts, inter, S(Metrics::HeroFontPx), &cfg);
    g_fonts.caps =
        AddFontIfValid(io.Fonts, mono, S(Metrics::CapsFontPx), &cfg);
    g_fonts.mono =
        AddFontIfValid(io.Fonts, mono, S(Metrics::MonoFontPx), &cfg);
    g_fonts.mono_small =
        AddFontIfValid(io.Fonts, mono, S(Metrics::MonoSmallFontPx), &cfg);
    g_fonts.icons =
        AddFontIfValid(io.Fonts, mono, S(Metrics::IconFontPx), &cfg);
#endif

#ifdef FW_FONT_DIR
    if (!g_fonts.body)
    {
      const std::string path = std::string(FW_FONT_DIR) + "/Roboto-Medium.ttf";
      g_fonts.body =
          AddFontIfValid(io.Fonts, path, S(Metrics::BodyFontPx), &cfg);
      g_fonts.large =
          AddFontIfValid(io.Fonts, path, S(Metrics::LargeFontPx), &cfg);
      g_fonts.hero =
          AddFontIfValid(io.Fonts, path, S(Metrics::HeroFontPx), &cfg);
    }
#endif

    if (!g_fonts.body)
    {
      g_fonts.body = io.Fonts->AddFontDefault();
    }
    if (!g_fonts.large)
    {
      g_fonts.large = g_fonts.body;
    }
    if (!g_fonts.hero)
    {
      g_fonts.hero = g_fonts.large;
    }
    if (!g_fonts.mono)
    {
      g_fonts.mono = g_fonts.body;
    }
    if (!g_fonts.caps)
    {
      g_fonts.caps = g_fonts.mono;
    }
    if (!g_fonts.mono_small)
    {
      g_fonts.mono_small = g_fonts.caps;
    }
    if (!g_fonts.icons)
    {
      g_fonts.icons = g_fonts.mono;
    }
    io.FontDefault = g_fonts.body;
  }

} // namespace fw::theme
