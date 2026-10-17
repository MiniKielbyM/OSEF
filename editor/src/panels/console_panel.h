#pragma once
// Build/run log (bottom).

#include "app/panel.h"

namespace osef::editor {

class ConsolePanel final : public Panel {
public:
    ConsolePanel();
    ~ConsolePanel() override;
    const char* title() const override { return "Console"; }
    void draw(EditorContext& ctx) override;

private:
    // Implementation details are owned by console_panel.cpp; add members freely.
    struct State;
    State* state_;
};

} // namespace osef::editor
