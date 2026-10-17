#pragma once
// The OS component tree (left).

#include "app/panel.h"

namespace osef::editor {

class HierarchyPanel final : public Panel {
public:
    HierarchyPanel();
    ~HierarchyPanel() override;
    const char* title() const override { return "Hierarchy"; }
    void draw(EditorContext& ctx) override;
    // Rows are full width (Unity style); the panel lays out its own margins.
    bool zero_padding() const override { return true; }

private:
    // Implementation details are owned by hierarchy_panel.cpp; add members freely.
    struct State;
    State* state_;
};

} // namespace osef::editor
