// Fixture: a valid BIOS declaration. Must compile (freestanding) and
// `osef-image inspect` must report firmware=BIOS.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("FixtureBIOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial | osef::Hw::PS2Keyboard)
    .memory_mb(48)
    .serial_baud(38400));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
