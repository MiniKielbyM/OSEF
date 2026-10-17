#pragma once
// The built-in shell, packaged as an optional module.
// Registers the "sh" program. Leave it out to ship your own userland.

#include <osef/module.h>

namespace osef {

class ShellModule final : public Module {
public:
    const char* name() const override { return "shell"; }
    void on_load(Kernel& kernel) override;
};

} // namespace osef
