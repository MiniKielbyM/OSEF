#pragma once
// File browser for the OS project and the engine (bottom).

#include "app/panel.h"

namespace osef::editor {

class ProjectPanel final : public Panel {
public:
    ProjectPanel();
    ~ProjectPanel() override;
    const char* title() const override { return "Project"; }
    void draw(EditorContext& ctx) override;

private:
    // Implementation details are owned by project_panel.cpp; add members freely.
    struct State;
    State* state_;
};

} // namespace osef::editor
