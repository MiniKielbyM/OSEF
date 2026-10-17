// Fixture: the declaration does not choose BIOS or UEFI.
// Must fail to compile with "OSEF_HARDWARE: choose the boot firmware".

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("NoFirmwareOS")
    .arch(osef::Arch::x86_64)
    .devices(osef::Hw::Serial)
    .memory_mb(64));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
