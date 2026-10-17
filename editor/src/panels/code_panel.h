#pragma once
// Tabbed code editor (center).

#include "app/panel.h"

namespace osef::editor {

class CodePanel final : public Panel {
public:
    CodePanel();
    ~CodePanel() override;
    const char* title() const override { return "Code"; }
    void draw(EditorContext& ctx) override;
    bool zero_padding() const override { return true; }

private:
    // Implementation details are owned by code_panel.cpp; add members freely.
    struct State;
    State* state_;
};

} // namespace osef::editor
