#pragma once
// Details of the selected component (right).

#include "app/panel.h"

namespace osef::editor {

class InspectorPanel final : public Panel {
public:
    InspectorPanel();
    ~InspectorPanel() override;
    const char* title() const override { return "Inspector"; }
    void draw(EditorContext& ctx) override;
    // Section headers span the full width (Unity style); the panel lays out its own margins.
    bool zero_padding() const override { return true; }

private:
    // Implementation details are owned by inspector_panel.cpp; add members freely.
    struct State;
    State* state_;
};

} // namespace osef::editor
