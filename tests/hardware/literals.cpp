// Fixture: a valid declaration after C++ that confuses naive scanners:
// digit separators (an odd number of them), a raw string with quotes and
// parentheses, character literals and a string mentioning OSEF_HARDWARE(.
// check-source must find the real declaration and the name exactly 31
// characters long must be accepted.

#include <osef/hardware.h>

static constexpr unsigned kHeapKiB = 64'000;
static constexpr unsigned long kMask = 0xFFFF'FFFF'FFFFul;
static const char* const kRaw = R"osef(he said "OSEF_HARDWARE(" ' and )" too)osef";
static const char kQuote = '\'';
static const char* const kText = "OSEF_HARDWARE(osef::Hardware().name(\"Wrong\"))";

OSEF_HARDWARE(osef::Hardware()
    .name("Literals" "OS-abcdefghijklmnopqrst")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::UEFI)
    .devices(osef::Hw::Serial));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    (void)kHeapKiB;
    (void)kMask;
    (void)kRaw;
    (void)kQuote;
    (void)kText;
    return kernel.run();
}
