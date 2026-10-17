// FaultOS: boot-test OS for bare-metal fault isolation and line input
// (tests/iso/fault_test.sh, run by `make test-iso`). Each program below
// crashes in a different way; every crash must kill only that process.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("FaultOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)
    .memory_mb(64));

#include <osef/osef.h>

using namespace osef;

namespace {

// Recurses until the kernel stack (256 KiB) runs into its guard page.
__attribute__((noinline)) int deep(int n) {
    volatile char frame[256];
    frame[0] = static_cast<char>(n);
    if (n == 0) return frame[0];
    return deep(n - 1) + frame[0];
}

volatile int g_zero = 0;

} // namespace

int main() {
    Kernel kernel;
    kernel.add_module<ShellModule>();
    auto& programs = kernel.programs();
    programs.add_fn("overflow", "infinite recursion: stack overflow", [](Context& c) {
        c.println(format("deep: %d", deep(1 << 30)));
        c.exit(0);
    });
    programs.add_fn("pf", "writes to an unmapped address: page fault", [](Context& c) {
        *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
        c.exit(0);
    });
    programs.add_fn("ud", "executes ud2: invalid opcode", [](Context& c) {
        asm volatile("ud2");
        c.exit(0);
    });
    programs.add_fn("div", "divides by zero: divide error", [](Context& c) {
        unsigned eax = 1, edx = 0;
        const unsigned divisor = static_cast<unsigned>(g_zero);
        asm volatile("divl %2" : "+a"(eax), "+d"(edx) : "r"(divisor));
        c.println(format("%u", eax));
        c.exit(0);
    });
    return kernel.run();
}
