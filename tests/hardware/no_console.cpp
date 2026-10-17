// Fixture: devices without any console (keyboard only).
// Must fail to compile with "OSEF_HARDWARE: enable at least one console device".

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("NoConsoleOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::UEFI)
    .devices(osef::Hw::PS2Keyboard)
    .memory_mb(64));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
