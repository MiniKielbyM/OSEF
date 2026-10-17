// Fixture: .name() longer than 31 characters. check-source must reject it
// with one line naming the length and the limit (the compiler's own error is
// a cascade of constexpr diagnostics).

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("NewOS The Most Amazing Operating System Ever")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
