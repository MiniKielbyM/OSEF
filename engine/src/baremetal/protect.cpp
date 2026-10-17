// Fault isolation: protected_call()/protected_fault() and CPU exception
// routing. A CPU exception raised while a protected_call is active (and not
// inside an IRQ handler) longjmps back to that protected_call with a
// descriptive message; anything else reaches the panic screen.

#include "cpu.h"
#include "internal.h"

extern "C" {
using osef_jmp_buf = uint64_t[8];
int osef_setjmp(osef_jmp_buf env) __attribute__((returns_twice));
[[noreturn]] void osef_longjmp(osef_jmp_buf env, int value);
}

namespace osef::bm {

namespace {

struct ProtectedRegion {
    osef_jmp_buf env;
    char* msg;
    size_t msg_size;
    ProtectedRegion* prev;
};

ProtectedRegion* volatile g_region = nullptr;
// Page faults run on their own IST stack (gdt.cpp). A page fault inside the
// page-fault handler would restart at the top of that same stack, on top of
// the frame still in use, so it must go straight to the panic screen.
volatile bool g_in_page_fault = false;

constexpr const char* kExceptionNames[32] = {
    "divide error",                 // 0  #DE
    "debug exception",              // 1  #DB
    "non-maskable interrupt",       // 2
    "breakpoint",                   // 3  #BP
    "overflow",                     // 4  #OF
    "bound range exceeded",         // 5  #BR
    "invalid opcode",               // 6  #UD
    "device not available",         // 7  #NM
    "double fault",                 // 8  #DF
    "coprocessor segment overrun",  // 9
    "invalid TSS",                  // 10 #TS
    "segment not present",          // 11 #NP
    "stack-segment fault",          // 12 #SS
    "general protection fault",     // 13 #GP
    "page fault",                   // 14 #PF
    "reserved exception 15",        // 15
    "x87 floating-point error",     // 16 #MF
    "alignment check",              // 17 #AC
    "machine check",                // 18 #MC
    "SIMD floating-point error",    // 19 #XM
    "virtualization exception",     // 20 #VE
    "control protection fault",     // 21 #CP
    "reserved exception 22",        "reserved exception 23", "reserved exception 24",
    "reserved exception 25",        "reserved exception 26", "reserved exception 27",
    "hypervisor injection",         // 28 #HV
    "VMM communication exception",  // 29 #VC
    "security exception",           // 30 #SX
    "reserved exception 31",
};

bool has_error_code(uint64_t v) {
    return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30;
}

// longjmp restores the stack pointer saved by protected_call (on the kernel
// stack, above any overflow), so this also leaves an IST stack correctly.
[[noreturn]] void unwind_to(ProtectedRegion* r) {
    g_in_page_fault = false;
    osef_longjmp(r->env, 1);
}

} // namespace

const char* exception_name(uint64_t vector) { return vector < 32 ? kExceptionNames[vector] : "interrupt"; }

extern "C" char __osef_stack_guard_start[], __osef_stack_bottom[];  // linker.ld

bool in_stack_guard(uint64_t addr) {
    return addr >= reinterpret_cast<uint64_t>(__osef_stack_guard_start) &&
           addr < reinterpret_cast<uint64_t>(__osef_stack_bottom);
}

bool protected_call(void (*fn)(void*), void* arg, char* msg, size_t msg_size) {
    ProtectedRegion region;
    region.msg = msg;
    region.msg_size = msg_size;
    region.prev = g_region;
    const bool irqs_were_on = cpu::interrupts_enabled();
    if (msg && msg_size) msg[0] = '\0';
    g_region = &region;
    if (osef_setjmp(region.env) == 0) {
        fn(arg);
        g_region = region.prev;
        return true;
    }
    // Arrived here from a CPU exception (interrupts off) or protected_fault().
    g_region = region.prev;
    if (irqs_were_on) cpu::sti();
    return false;
}

void protected_fault(const char* msg) {
    ProtectedRegion* r = g_region;
    if (!r) {
        char buf[256];
        fmt(buf, sizeof buf, "fault outside a protected region: %s", msg ? msg : "(no message)");
        panic(buf);
    }
    if (r->msg && r->msg_size) copy_cstr(r->msg, r->msg_size, msg ? msg : "fault");
    unwind_to(r);
}

void handle_exception(InterruptFrame* frame) {
    const uint64_t v = frame->vector;
    if (v == 2) {  // NMI: report and carry on
        emergency_write("[osef] warning: non-maskable interrupt (hardware error?)\n");
        return;
    }
    if (v == 15) {
        // Reserved: the CPU never raises vector 15, but firmware commonly
        // programs it as the local-APIC spurious vector (SVR = 0x..0F), and
        // a spurious interrupt must be ignored (no EOI).
        return;
    }
    if (v == 14) {
        if (g_in_page_fault) panic_exception(*frame);  // nested: see g_in_page_fault
        g_in_page_fault = true;
    }
    ProtectedRegion* r = g_region;
    // Recoverable only if a protected region is active, we are not inside an
    // IRQ handler (its EOI would be lost), we are not already panicking (a
    // fault in the panic path -- possibly on the #DF IST stack -- must not
    // resume the OS) and the CPU state is trustworthy.
    if (r && g_irq_depth == 0 && !panicking() && v != 8 && v != 18) {
        if (r->msg && r->msg_size) {
            if (v == 14 && in_stack_guard(cpu::read_cr2())) {
                fmt(r->msg, r->msg_size, "stack overflow (page fault at 0x%016llx below the kernel stack, rip=0x%016llx)",
                    static_cast<unsigned long long>(cpu::read_cr2()), static_cast<unsigned long long>(frame->rip));
            } else if (v == 14) {
                fmt(r->msg, r->msg_size, "page fault at 0x%016llx (rip=0x%016llx, error=0x%llx)",
                    static_cast<unsigned long long>(cpu::read_cr2()), static_cast<unsigned long long>(frame->rip),
                    static_cast<unsigned long long>(frame->error_code));
            } else if (has_error_code(v)) {
                fmt(r->msg, r->msg_size, "%s (rip=0x%016llx, error=0x%llx)", exception_name(v),
                    static_cast<unsigned long long>(frame->rip), static_cast<unsigned long long>(frame->error_code));
            } else {
                fmt(r->msg, r->msg_size, "%s (rip=0x%016llx)", exception_name(v),
                    static_cast<unsigned long long>(frame->rip));
            }
        }
        unwind_to(r);
    }
    panic_exception(*frame);
}

} // namespace osef::bm
