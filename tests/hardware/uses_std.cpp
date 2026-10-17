// Fixture: an OS that uses the C++ standard library outside #if OSEF_HOSTED.
// check-source must warn (hosted builds would otherwise succeed silently),
// hosted-only std use must not be reported, and the freestanding compile
// must fail.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("UsesStd")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial));

#include <osef/osef.h>

#include <string>

#if OSEF_HOSTED
#include <cstdio>
[[maybe_unused]] static void hosted_only() { std::puts("fine: hosted only"); }
#endif

int main() {
    std::string greeting = "hello";
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    return kernel.run() + static_cast<int>(greeting.size()) * 0;
}
