// Fixture (hosted compile): the public OSEF headers must not make the C/C++
// standard library visible, or OS code using it without an #include builds
// hosted and only fails in the ISO build. Compiled hosted with
// -DOSEF_LEAK_CHECK=<n>: every variant must fail to compile.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("NoStdLeak")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial));

#include <osef/osef.h>

#if OSEF_LEAK_CHECK == 1
std::string leak_string() { return "std::string without #include <string>"; }
#elif OSEF_LEAK_CHECK == 2
int leak_printf() { return printf("printf without #include <stdio.h>\n"); }
#elif OSEF_LEAK_CHECK == 3
std::vector<int> leak_vector() { return {}; }
#elif OSEF_LEAK_CHECK == 4
std::filesystem::path leak_filesystem() { return "/"; }
#else
int ok() { return 0; }  // the baseline compiles
#endif

int main() { return 0; }
