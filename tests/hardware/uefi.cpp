// Fixture: a valid UEFI declaration. Must compile (freestanding) and
// `osef-image inspect` must report firmware=UEFI.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("FixtureUEFI")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::UEFI)
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer)
    .memory_mb(96)
    .resolution(1024, 768)
    .boot_timeout(2));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
