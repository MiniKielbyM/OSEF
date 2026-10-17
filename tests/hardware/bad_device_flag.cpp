// Fixture: an unknown device flag.
// Must fail to compile, naming OSEF_HARDWARE_unknown_device_flag.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("BadDeviceOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial | (1u << 9))
    .memory_mb(64));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
