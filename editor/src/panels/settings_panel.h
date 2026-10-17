#pragma once
// OS Settings: the beginner-friendly way to customize an OS without code.
// Start-up (name, PC type, memory), look & feel with a live preview, hardware,
// programs from templates, and files. Edits are saved right away.

#include "app/panel.h"

namespace osef::editor {

class SettingsPanel final : public Panel {
public:
    SettingsPanel();
    ~SettingsPanel() override;
    const char* title() const override { return "OS Settings"; }
    void draw(EditorContext& ctx) override;

    struct State;  // public so the panel's helper functions can take it

private:
    State* state_;
};

} // namespace osef::editor
