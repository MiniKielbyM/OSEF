// Fixture: an OS that forgot its hardware declaration.
// `osef-image check-source` must reject it. The mention of
// OSEF_HARDWARE(...) in this comment must not count as a declaration.

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
