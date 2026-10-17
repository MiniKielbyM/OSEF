// The smallest possible OSEF operating system: the engine plus its shell.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("TinyOS")
    .arch(osef::Arch::x86_64)                  // required
    .firmware(osef::Firmware::BIOS)            // required: BIOS or UEFI
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)
    .memory_mb(64));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel; // the OS name comes from the hardware declaration
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
