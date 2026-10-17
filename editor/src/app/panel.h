#pragma once
// Base class for every dockable editor window (Hierarchy, Code, Inspector, ...).
// To add a panel: subclass Panel, then register it in EditorApp::EditorApp().

namespace osef::editor {

struct EditorContext;

class Panel {
public:
    virtual ~Panel() = default;

    // Stable ImGui window name. It is the docking/layout key and the label
    // in the Window menu, so don't change it casually.
    virtual const char* title() const = 0;

    // Draws the panel contents. EditorApp calls ImGui::Begin(title(), &visible,
    // window_flags()) before this and ImGui::End() after it.
    virtual void draw(EditorContext& ctx) = 0;

    // Extra ImGuiWindowFlags for ImGui::Begin (e.g. ImGuiWindowFlags_MenuBar).
    virtual int window_flags() const { return 0; }

    // Zero padding lets panels like the code editor draw edge to edge.
    virtual bool zero_padding() const { return false; }

    bool visible = true;
};

} // namespace osef::editor
