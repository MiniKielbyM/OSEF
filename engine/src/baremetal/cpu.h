#pragma once
// x86_64 CPU primitives (port I/O, control registers, interrupts, TSC, CPUID).

#include <stdint.h>

namespace osef::bm::cpu {

inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port) : "memory"); }
inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port) : "memory"); }
inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(port) : "memory"); }
inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline uint16_t inw(uint16_t port) {
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline uint32_t inl(uint16_t port) {
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
// ~1 us delay (write to the POST diagnostic port).
inline void io_wait() { outb(0x80, 0); }

inline void cli() { asm volatile("cli" : : : "memory"); }
inline void sti() { asm volatile("sti" : : : "memory"); }
inline void hlt() { asm volatile("hlt" : : : "memory"); }
inline void pause() { asm volatile("pause" : : : "memory"); }
inline void sti_hlt() { asm volatile("sti; hlt" : : : "memory"); }

[[noreturn]] inline void halt_forever() {
    for (;;) asm volatile("cli; hlt" : : : "memory");
}

inline uint64_t read_rflags() {
    uint64_t f;
    asm volatile("pushfq; popq %0" : "=r"(f) : : "memory");
    return f;
}
inline bool interrupts_enabled() { return (read_rflags() & (1u << 9)) != 0; }

inline uint64_t read_cr0() { uint64_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
inline uint64_t read_cr2() { uint64_t v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v; }
inline uint64_t read_cr3() { uint64_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
inline uint64_t read_cr4() { uint64_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
inline void write_cr0(uint64_t v) { asm volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
inline void write_cr4(uint64_t v) { asm volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

struct CpuidResult { uint32_t eax, ebx, ecx, edx; };
inline CpuidResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
    CpuidResult r;
    asm volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
    return r;
}

// CR0.EM=0, CR0.MP=1, CR0.TS=0, CR4.OSFXSR|OSXMMEXCPT; then a clean x87/SSE state.
// Pure inline asm: safe to call before any compiler-generated SSE code runs.
inline void enable_sse() {
    asm volatile(
        "mov %%cr0, %%rax\n\t"
        "and $~0xC, %%rax\n\t"      // clear EM (bit 2) and TS (bit 3)
        "or $0x2, %%rax\n\t"        // set MP (bit 1)
        "mov %%rax, %%cr0\n\t"
        "mov %%cr4, %%rax\n\t"
        "or $0x600, %%rax\n\t"      // OSFXSR (bit 9) | OSXMMEXCPT (bit 10)
        "mov %%rax, %%cr4\n\t"
        "fninit\n\t"
        "pushq $0x1F80\n\t"         // default MXCSR: all exceptions masked
        "ldmxcsr (%%rsp)\n\t"
        "addq $8, %%rsp\n\t"
        :
        :
        : "rax", "memory");
}

} // namespace osef::bm::cpu
