// Kernel panic screen and power off.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {

volatile int g_panicking = 0;
// QEMU isa-debug-exit (test VMs): writing v makes QEMU exit with status
// (v << 1) | 1. OSEF writes exit_code + 1 (codes 0..125 -> 1..126; others are
// clamped to 125) and 127 for a kernel panic, so v is never 0: QEMU's own
// error status 1 can never look like "the OS exited with code 0".
// osef-image run --test decodes it (tools/osef-image/main.cpp, kRun*).
constexpr uint16_t kQemuDebugExit = 0xF4;
constexpr uint8_t kPanicExitValue = 0x7F;  // QEMU status 255
constexpr int kMaxReportedExitCode = 125;

void emergency_serial(const char* s) {
    // Raw COM1 polling; works even if serial_init never ran or the UART is
    // half-configured (writes to an absent UART are harmless).
    for (; *s; ++s) {
        for (uint32_t spin = 0; spin < 100000 && !(cpu::inb(0x3F8 + 5) & 0x20); ++spin) cpu::pause();
        cpu::outb(0x3F8, static_cast<uint8_t>(*s));
    }
}

// Make sure every console that can show the panic is up.
void bring_up_consoles() {
    if (console_fb_enabled() && !fbcon_active()) fbcon_init(boot_framebuffer());
}

void out(const char* s) { console_write(s, cstr_len(s)); }

void outf(const char* f, ...) OSEF_BM_PRINTF(1, 2);
void outf(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    size_t n = vfmt(buf, sizeof buf, f, ap);
    va_end(ap);
    console_write(buf, n < sizeof buf ? n : sizeof buf - 1);
}

[[noreturn]] void stop() {
    out("\nSystem halted.\n");
    serial_flush();
    cpu::outb(kQemuDebugExit, kPanicExitValue);
    cpu::halt_forever();
}

bool enter_panic() {
    cpu::cli();
    if (g_panicking) return false;
    g_panicking = 1;
    return true;
}

void banner(const char* title) {
    bring_up_consoles();
    out("\n\x1b[0m\x1b[1;97;41m *** KERNEL PANIC *** \x1b[0m\n");
    out("\x1b[1;91m");
    out(title);
    out("\x1b[0m\n");
}

} // namespace

bool panicking() { return g_panicking != 0; }

void emergency_write(const char* s) {
    if (serial_present()) serial_write(s, cstr_len(s));
    else emergency_serial(s);
    if (fbcon_active()) fbcon_write(s, cstr_len(s));
}

void panic(const char* msg) {
    if (!enter_panic()) {
        emergency_serial("\n[osef] nested panic: ");
        emergency_serial(msg ? msg : "(null)");
        emergency_serial("\n");
        cpu::outb(kQemuDebugExit, kPanicExitValue);
        cpu::halt_forever();
    }
    banner(msg ? msg : "(no message)");
    stop();
}

void panic_exception(const InterruptFrame& f) {
    uint64_t cr2 = cpu::read_cr2();
    if (!enter_panic()) {
        emergency_serial("\n[osef] CPU exception during panic: ");
        emergency_serial(exception_name(f.vector));
        emergency_serial("\n");
        cpu::outb(kQemuDebugExit, kPanicExitValue);
        cpu::halt_forever();
    }
    char title[128];
    fmt(title, sizeof title, "Unhandled CPU exception %llu: %s", static_cast<unsigned long long>(f.vector),
        exception_name(f.vector));
    banner(title);
    if (f.vector == 14 && in_stack_guard(cr2)) out("  kernel stack overflow: the guard page below the stack was hit\n");
    if (f.vector == 14) {
        outf("  faulting address (cr2): 0x%016llx  [%s, %s, %s]\n", static_cast<unsigned long long>(cr2),
             (f.error_code & 1) ? "protection violation" : "page not present",
             (f.error_code & 2) ? "write" : ((f.error_code & 16) ? "instruction fetch" : "read"),
             (f.error_code & 4) ? "user" : "kernel");
    }
    outf("  error code: 0x%llx\n", static_cast<unsigned long long>(f.error_code));
    outf("  rip=0x%016llx  cs=0x%04llx  rflags=0x%016llx\n", static_cast<unsigned long long>(f.rip),
         static_cast<unsigned long long>(f.cs), static_cast<unsigned long long>(f.rflags));
    outf("  rsp=0x%016llx  ss=0x%04llx\n", static_cast<unsigned long long>(f.rsp),
         static_cast<unsigned long long>(f.ss));
    outf("  rax=0x%016llx  rbx=0x%016llx  rcx=0x%016llx\n", static_cast<unsigned long long>(f.rax),
         static_cast<unsigned long long>(f.rbx), static_cast<unsigned long long>(f.rcx));
    outf("  rdx=0x%016llx  rsi=0x%016llx  rdi=0x%016llx\n", static_cast<unsigned long long>(f.rdx),
         static_cast<unsigned long long>(f.rsi), static_cast<unsigned long long>(f.rdi));
    outf("  rbp=0x%016llx  r8 =0x%016llx  r9 =0x%016llx\n", static_cast<unsigned long long>(f.rbp),
         static_cast<unsigned long long>(f.r8), static_cast<unsigned long long>(f.r9));
    outf("  r10=0x%016llx  r11=0x%016llx  r12=0x%016llx\n", static_cast<unsigned long long>(f.r10),
         static_cast<unsigned long long>(f.r11), static_cast<unsigned long long>(f.r12));
    outf("  r13=0x%016llx  r14=0x%016llx  r15=0x%016llx\n", static_cast<unsigned long long>(f.r13),
         static_cast<unsigned long long>(f.r14), static_cast<unsigned long long>(f.r15));
    outf("  cr0=0x%016llx  cr2=0x%016llx\n", static_cast<unsigned long long>(cpu::read_cr0()),
         static_cast<unsigned long long>(cr2));
    outf("  cr3=0x%016llx  cr4=0x%016llx\n", static_cast<unsigned long long>(cpu::read_cr3()),
         static_cast<unsigned long long>(cpu::read_cr4()));
    stop();
}

void power_off(int exit_code) {
    char line[64];
    size_t n = fmt(line, sizeof line, "\nSystem halted (exit code %d).\n", exit_code);
    console_write(line, n < sizeof line ? n : sizeof line - 1);
    serial_flush();  // the poweroff below would cut off bytes still in the UART FIFO
    cpu::cli();
    const int reported = exit_code >= 0 && exit_code <= kMaxReportedExitCode ? exit_code : kMaxReportedExitCode;
    cpu::outb(kQemuDebugExit, static_cast<uint8_t>(reported + 1));  // QEMU isa-debug-exit
    if (acpi_power_off()) cpu::halt_forever();
    cpu::outw(0x604, 0x2000);   // QEMU (q35 / i440fx with modern firmware) ACPI PM1a
    cpu::outw(0xB004, 0x2000);  // Bochs / older QEMU
    cpu::outw(0x4004, 0x3400);  // VirtualBox
    cpu::halt_forever();
}

} // namespace osef::bm
