#pragma once
// Unity "dark skin" look: ImGui style, fonts and named colors shared by panels.

#include <filesystem>

#include "imgui.h"

namespace osef::editor {

struct Fonts;

namespace theme {

enum class Palette { Dark, Light, HighContrast };

enum class Color {
    Text, TextDim, TextDisabled,
    WindowBg, PanelBg, TitleBg, TabActive, TabInactive,
    SectionHeader, SectionHeaderHover, SectionBorder,
    Separator, Border,
    Selection, SelectionUnfocused, Hover,
    InputBg, InputBorder,
    Button, ButtonHover, ButtonActive,
    Accent,
    Implemented, Partial, Planned,
    Warning, Error, Info,
    CodeBg, CodeGutter, CodeLineNumber, CodeLineNumberActive, CodeCurrentLine, CodeSelection, CodeCursor,
    SynKeyword, SynControl, SynType, SynFunction, SynString, SynNumber, SynComment,
    SynPreprocessor, SynMacro, SynPunctuation, SynIdentifier,
    Count
};

ImU32 color(Color c);          // packed RGBA
ImVec4 color_vec(Color c);
Palette palette();
const char* palette_name(Palette palette);
void set_palette(Palette palette);

// Applies the style + color scheme to the current ImGui context, scaled by `scale`.
void apply_style(float scale = 1.0f);

// Loads Inter / JetBrains Mono from `fonts_dir` (falls back to ImGui's
// default font for anything missing) and fills `fonts`. `size` is the UI font
// size in pixels (code and small fonts are derived from it).
void load_fonts(const std::filesystem::path& fonts_dir, float size, Fonts& fonts);

} // namespace theme
} // namespace osef::editor
