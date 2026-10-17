#pragma once
// Vector icons drawn with ImDrawList (no icon font needed).

#include "imgui.h"
#include "model/component.h"

namespace osef::editor::icons {

enum class Glyph {
    Play, Stop, Build, Test,
    Plus, Search, Gear, Link, Lock, Unlock, ExternalLink, Close,
    Folder, FolderOpen, File, FileCode,
    Info, Warning, Error, Check, Dot,
};

// Draws `glyph` inside the square [min, min + (size, size)].
void draw(ImDrawList* dl, ImVec2 min, float size, Glyph glyph, ImU32 color);

// Draws the icon for a component kind (CPU chip, keyboard, mouse, USB, ...)
// inside the square [min, min + (size, size)].
void draw(ImDrawList* dl, ImVec2 min, float size, ComponentKind kind, ImU32 color);

// The default tint for a component kind's icon.
ImU32 kind_color(ComponentKind kind);

// Reserves a size x size item at the cursor (like ImGui::Dummy) and draws the icon there.
void item(Glyph glyph, float size, ImU32 color);
void item(ComponentKind kind, float size, ImU32 color);

} // namespace osef::editor::icons
