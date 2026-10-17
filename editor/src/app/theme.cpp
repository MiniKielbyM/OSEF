// Unity "dark skin" style, colors and fonts.

#include "app/theme.h"

#include "app/editor_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace osef::editor::theme {
namespace {

constexpr ImU32 rgb(uint32_t hex, uint32_t alpha = 0xFF) {
    // 0xRRGGBB -> ImGui's packed ABGR.
    return ((alpha & 0xFF) << 24) | ((hex & 0xFF) << 16) | (hex & 0xFF00) | ((hex >> 16) & 0xFF);
}

// Indexed by Color. Values from ARCHITECTURE.md ("Look and feel").
constexpr ImU32 kColors[] = {
    rgb(0xD2D2D2),  // Text
    rgb(0x8A8A8A),  // TextDim
    rgb(0x6C6C6C),  // TextDisabled
    rgb(0x383838),  // WindowBg
    rgb(0x383838),  // PanelBg
    rgb(0x282828),  // TitleBg
    rgb(0x3C3C3C),  // TabActive
    rgb(0x282828),  // TabInactive
    rgb(0x3E3E3E),  // SectionHeader
    rgb(0x474747),  // SectionHeaderHover
    rgb(0x232323),  // SectionBorder
    rgb(0x191919),  // Separator
    rgb(0x232323),  // Border
    rgb(0x2C5D87),  // Selection
    rgb(0x4D4D4D),  // SelectionUnfocused
    rgb(0x454545),  // Hover
    rgb(0x2A2A2A),  // InputBg
    rgb(0x212121),  // InputBorder
    rgb(0x585858),  // Button
    rgb(0x676767),  // ButtonHover
    rgb(0x464646),  // ButtonActive
    rgb(0x4C7EFF),  // Accent
    rgb(0x5DB35D),  // Implemented
    rgb(0xD8A23A),  // Partial
    rgb(0x8A8A8A),  // Planned
    rgb(0xE8B93A),  // Warning
    rgb(0xE05252),  // Error
    rgb(0xC4C4C4),  // Info
    rgb(0x1E1E1E),  // CodeBg
    rgb(0x1E1E1E),  // CodeGutter
    rgb(0x858585),  // CodeLineNumber
    rgb(0xC6C6C6),  // CodeLineNumberActive
    rgb(0x282828),  // CodeCurrentLine
    rgb(0x264F78),  // CodeSelection
    rgb(0xAEAFAD),  // CodeCursor
    rgb(0x569CD6),  // SynKeyword
    rgb(0xC586C0),  // SynControl
    rgb(0x4EC9B0),  // SynType
    rgb(0xDCDCAA),  // SynFunction
    rgb(0xCE9178),  // SynString
    rgb(0xB5CEA8),  // SynNumber
    rgb(0x6A9955),  // SynComment
    rgb(0xC586C0),  // SynPreprocessor
    rgb(0x4FC1FF),  // SynMacro
    rgb(0xD4D4D4),  // SynPunctuation
    rgb(0xD4D4D4),  // SynIdentifier
};
static_assert(sizeof(kColors) / sizeof(kColors[0]) == static_cast<size_t>(Color::Count),
              "kColors must have one entry per theme::Color");

constexpr size_t kColorCount = static_cast<size_t>(Color::Count);
constexpr size_t color_index(Color c) { return static_cast<size_t>(c); }

std::array<ImU32, kColorCount> make_palette(bool high_contrast) {
    std::array<ImU32, kColorCount> colors{};
    std::copy(std::begin(kColors), std::end(kColors), colors.begin());
    auto set = [&](Color c, uint32_t hex) { colors[color_index(c)] = rgb(hex); };
    if (high_contrast) {
        set(Color::Text, 0xFFFFFF); set(Color::TextDim, 0xDFDFDF); set(Color::TextDisabled, 0xB0B0B0);
        set(Color::WindowBg, 0x000000); set(Color::PanelBg, 0x000000); set(Color::TitleBg, 0x111111);
        set(Color::TabActive, 0x111111); set(Color::TabInactive, 0x050505);
        set(Color::SectionHeader, 0x1A1A1A); set(Color::SectionHeaderHover, 0x303030);
        set(Color::SectionBorder, 0x707070); set(Color::Separator, 0x808080); set(Color::Border, 0xA0A0A0);
        set(Color::Selection, 0x003E54); set(Color::SelectionUnfocused, 0x303030); set(Color::Hover, 0x282828);
        set(Color::InputBg, 0x101010); set(Color::InputBorder, 0xAAAAAA);
        set(Color::Button, 0x242424); set(Color::ButtonHover, 0x383838); set(Color::ButtonActive, 0x505050);
        set(Color::Accent, 0x00E5FF); set(Color::Implemented, 0x00FF66); set(Color::Partial, 0xFFD600);
        set(Color::Planned, 0xBFBFBF); set(Color::Warning, 0xFFD600); set(Color::Error, 0xFF6666);
        set(Color::Info, 0xFFFFFF); set(Color::CodeBg, 0x000000); set(Color::CodeGutter, 0x000000);
        set(Color::CodeLineNumber, 0xB0B0B0); set(Color::CodeLineNumberActive, 0xFFFFFF);
        set(Color::CodeCurrentLine, 0x202020); set(Color::CodeSelection, 0x004D66);
        set(Color::CodeCursor, 0xFFFFFF); set(Color::SynKeyword, 0x66CCFF); set(Color::SynControl, 0xFF99FF);
        set(Color::SynType, 0x66FFCC); set(Color::SynFunction, 0xFFFF99); set(Color::SynString, 0xFFB380);
        set(Color::SynNumber, 0xCCFF99); set(Color::SynComment, 0x99CC66); set(Color::SynPreprocessor, 0xFF99FF);
        set(Color::SynMacro, 0x66DDFF); set(Color::SynPunctuation, 0xFFFFFF); set(Color::SynIdentifier, 0xFFFFFF);
        return colors;
    }

    set(Color::Text, 0x202124); set(Color::TextDim, 0x5F6368); set(Color::TextDisabled, 0x80868B);
    set(Color::WindowBg, 0xF1F3F5); set(Color::PanelBg, 0xF1F3F5); set(Color::TitleBg, 0xE3E7EB);
    set(Color::TabActive, 0xF1F3F5); set(Color::TabInactive, 0xDDE2E7);
    set(Color::SectionHeader, 0xE7EAEE); set(Color::SectionHeaderHover, 0xD8DEE5);
    set(Color::SectionBorder, 0xD0D5DB); set(Color::Separator, 0xC5CBD2); set(Color::Border, 0xB9C0C8);
    set(Color::Selection, 0xCFE2FF); set(Color::SelectionUnfocused, 0xE1E5EA); set(Color::Hover, 0xDCE5EF);
    set(Color::InputBg, 0xFFFFFF); set(Color::InputBorder, 0xB8C0C8);
    set(Color::Button, 0xDCE1E6); set(Color::ButtonHover, 0xCCD4DC); set(Color::ButtonActive, 0xBCC7D1);
    set(Color::Accent, 0x245FC0); set(Color::Implemented, 0x238636); set(Color::Partial, 0x9A6700);
    set(Color::Planned, 0x707780); set(Color::Warning, 0x9A6700); set(Color::Error, 0xCF222E);
    set(Color::Info, 0x49566A); set(Color::CodeBg, 0xFFFFFF); set(Color::CodeGutter, 0xF1F3F5);
    set(Color::CodeLineNumber, 0x6E7781); set(Color::CodeLineNumberActive, 0x202124);
    set(Color::CodeCurrentLine, 0xEEF4FB); set(Color::CodeSelection, 0xB9D5F7);
    set(Color::CodeCursor, 0x202124); set(Color::SynKeyword, 0x0550AE); set(Color::SynControl, 0x8250DF);
    set(Color::SynType, 0x116329); set(Color::SynFunction, 0x8250DF); set(Color::SynString, 0x0A3069);
    set(Color::SynNumber, 0x0550AE); set(Color::SynComment, 0x6E7781); set(Color::SynPreprocessor, 0x8250DF);
    set(Color::SynMacro, 0x953800); set(Color::SynPunctuation, 0x424A53); set(Color::SynIdentifier, 0x202124);
    return colors;
}

const std::array<ImU32, kColorCount> kLightColors = make_palette(false);
const std::array<ImU32, kColorCount> kHighContrastColors = make_palette(true);
Palette g_palette = Palette::Dark;
float g_style_scale = 1.0f;

ImVec4 vec(uint32_t hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

ImFont* add_font(const std::filesystem::path& file, float size, bool merge = false) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.MergeMode = merge;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(file.string().c_str(), size, &cfg);
}

} // namespace

ImU32 color(Color c) {
    const auto i = static_cast<size_t>(c);
    if (i >= kColorCount) return IM_COL32(255, 0, 255, 255);
    if (g_palette == Palette::Light) return kLightColors[i];
    if (g_palette == Palette::HighContrast) return kHighContrastColors[i];
    return kColors[i];
}

ImVec4 color_vec(Color c) { return ImGui::ColorConvertU32ToFloat4(color(c)); }

Palette palette() { return g_palette; }

const char* palette_name(Palette value) {
    switch (value) {
    case Palette::Dark: return "OSEF Dark";
    case Palette::Light: return "Light";
    case Palette::HighContrast: return "High Contrast";
    }
    return "OSEF Dark";
}

void set_palette(Palette value) {
    if (g_palette == value) return;
    g_palette = value;
    apply_style(g_style_scale);
}

void apply_style(float scale) {
    g_style_scale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    // Start from defaults but keep the font scaling the app already set up.
    const float font_size_base = s.FontSizeBase;
    const float font_scale_main = s.FontScaleMain;
    s = ImGuiStyle();
    s.FontSizeBase = font_size_base;
    s.FontScaleMain = font_scale_main;
    s.FontScaleDpi = scale;

    s.WindowPadding = ImVec2(6.0f, 6.0f);
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 1.0f;
    s.WindowMinSize = ImVec2(64.0f, 48.0f);
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;  // Unity tabs have no collapse arrow
    s.ChildRounding = 2.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupRounding = 3.0f;
    s.PopupBorderSize = 1.0f;
    s.FramePadding = ImVec2(6.0f, 3.0f);
    s.FrameRounding = 3.0f;
    s.FrameBorderSize = 1.0f;
    s.ItemSpacing = ImVec2(6.0f, 4.0f);
    s.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    s.CellPadding = ImVec2(4.0f, 2.0f);
    s.IndentSpacing = 14.0f;
    s.ColumnsMinSpacing = 6.0f;
    s.ScrollbarSize = 12.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabMinSize = 10.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.TabBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 1.0f;
    s.TabMinWidthBase = 1.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.DockingSeparatorSize = 2.0f;
    s.DockingNodeHasCloseButton = false;  // close panels from their tab or the Window menu
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = color_vec(Color::Text);
    c[ImGuiCol_TextDisabled] = color_vec(Color::TextDim);
    c[ImGuiCol_WindowBg] = color_vec(Color::WindowBg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = color_vec(Color::PanelBg);
    c[ImGuiCol_Border] = color_vec(Color::Border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = color_vec(Color::InputBg);
    c[ImGuiCol_FrameBgHovered] = color_vec(Color::Hover);
    c[ImGuiCol_FrameBgActive] = color_vec(Color::SelectionUnfocused);
    c[ImGuiCol_TitleBg] = color_vec(Color::TitleBg);
    c[ImGuiCol_TitleBgActive] = color_vec(Color::TitleBg);
    c[ImGuiCol_TitleBgCollapsed] = color_vec(Color::TitleBg);
    c[ImGuiCol_MenuBarBg] = color_vec(Color::TitleBg);
    c[ImGuiCol_ScrollbarBg] = color_vec(Color::PanelBg);
    c[ImGuiCol_ScrollbarBg].w = 0.0f;
    c[ImGuiCol_ScrollbarGrab] = color_vec(Color::TextDim);
    c[ImGuiCol_ScrollbarGrabHovered] = color_vec(Color::Text);
    c[ImGuiCol_ScrollbarGrabActive] = color_vec(Color::Accent);
    c[ImGuiCol_CheckMark] = color_vec(Color::Accent);
    c[ImGuiCol_CheckboxSelectedBg] = color_vec(Color::InputBg);
    c[ImGuiCol_SliderGrab] = color_vec(Color::Accent);
    c[ImGuiCol_SliderGrabActive] = vec(0x6E96FF);
    c[ImGuiCol_Button] = color_vec(Color::Button);
    c[ImGuiCol_ButtonHovered] = color_vec(Color::ButtonHover);
    c[ImGuiCol_ButtonActive] = color_vec(Color::ButtonActive);
    c[ImGuiCol_Header] = color_vec(Color::Selection);
    c[ImGuiCol_HeaderHovered] = color_vec(Color::Hover);
    c[ImGuiCol_HeaderActive] = color_vec(Color::Selection);
    c[ImGuiCol_Separator] = color_vec(Color::Separator);
    c[ImGuiCol_SeparatorHovered] = vec(0x4C7EFF, 0.65f);
    c[ImGuiCol_SeparatorActive] = color_vec(Color::Accent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = vec(0x4C7EFF, 0.5f);
    c[ImGuiCol_ResizeGripActive] = color_vec(Color::Accent);
    c[ImGuiCol_InputTextCursor] = color_vec(Color::Text);
    c[ImGuiCol_TabHovered] = color_vec(Color::Hover);
    c[ImGuiCol_Tab] = color_vec(Color::TabInactive);
    c[ImGuiCol_TabSelected] = color_vec(Color::TabActive);
    c[ImGuiCol_TabSelectedOverline] = color_vec(Color::Accent);
    c[ImGuiCol_TabDimmed] = color_vec(Color::TabInactive);
    c[ImGuiCol_TabDimmedSelected] = color_vec(Color::TabActive);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = vec(0x4C7EFF, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = color_vec(Color::WindowBg);
    c[ImGuiCol_PlotLines] = vec(0x9C9C9C);
    c[ImGuiCol_PlotLinesHovered] = color_vec(Color::Accent);
    c[ImGuiCol_PlotHistogram] = color_vec(Color::Accent);
    c[ImGuiCol_PlotHistogramHovered] = vec(0x6E96FF);
    c[ImGuiCol_TableHeaderBg] = color_vec(Color::SectionHeader);
    c[ImGuiCol_TableBorderStrong] = color_vec(Color::Border);
    c[ImGuiCol_TableBorderLight] = color_vec(Color::SectionBorder);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextLink] = color_vec(Color::Accent);
    c[ImGuiCol_TextSelectedBg] = color_vec(Color::Selection);
    c[ImGuiCol_TreeLines] = color_vec(Color::TextDim);
    c[ImGuiCol_DragDropTarget] = color_vec(Color::Accent);
    c[ImGuiCol_DragDropTargetBg] = color_vec(Color::Selection);
    c[ImGuiCol_UnsavedMarker] = color_vec(Color::Text);
    c[ImGuiCol_NavCursor] = color_vec(Color::Accent);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);

    s.ScaleAllSizes(scale);
}

void load_fonts(const std::filesystem::path& fonts_dir, float size, Fonts& fonts) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const float mono_size = std::round(size * 0.93f);
    const float small_size = std::round(size * 0.85f);
    const auto inter = fonts_dir.empty() ? std::filesystem::path() : fonts_dir / "Inter-Regular.ttf";
    const auto inter_bold = fonts_dir.empty() ? std::filesystem::path() : fonts_dir / "Inter-SemiBold.ttf";
    const auto mono = fonts_dir.empty() ? std::filesystem::path() : fonts_dir / "JetBrainsMono-Regular.ttf";

    // Inter lacks some symbols the UI uses (arrows, box drawing, geometric
    // shapes); JetBrains Mono is merged in as a per-glyph fallback.
    auto ui_font = [&](const std::filesystem::path& file, float px) -> ImFont* {
        ImFont* f = add_font(file, px);
        if (f) add_font(mono, px, /*merge=*/true);
        return f;
    };

    fonts = Fonts{};
    fonts.ui = ui_font(inter, size);  // the first font added is ImGui's default
    fonts.ui_bold = ui_font(inter_bold, size);
    fonts.mono = add_font(mono, mono_size);
    fonts.small = ui_font(inter, small_size);

    ImFont* fallback = nullptr;
    auto get_fallback = [&](float px) -> ImFont* {
        // Built-in scalable font; one instance serves every role (sizes are dynamic).
        if (!fallback) {
            ImFontConfig cfg;
            cfg.SizePixels = px;
            fallback = atlas->AddFontDefault(&cfg);
        }
        return fallback;
    };
    if (!fonts.ui) fonts.ui = get_fallback(size);
    if (!fonts.ui_bold) fonts.ui_bold = fonts.ui;
    if (!fonts.mono) fonts.mono = get_fallback(size);
    if (!fonts.small) fonts.small = fonts.ui;
    ImGui::GetIO().FontDefault = fonts.ui;
}

} // namespace osef::editor::theme
