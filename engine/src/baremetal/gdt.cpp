// GDT (null, kernel code 0x08, kernel data 0x10, TSS 0x18) and the TSS with
// two interrupt stacks: IST1 for double faults and IST2 for page faults. A
// page fault caused by a stack overflow (the guard below the kernel stack)
// cannot push its frame on the exhausted stack; on IST2 it still runs, so
// the overflow becomes a process fault or a readable panic, not #DF / reset.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {

struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(Tss) == 104, "TSS layout");

struct __attribute__((packed)) Gdtr {
    uint16_t limit;
    uint64_t base;
};

constexpr size_t kDoubleFaultStackSize = 16 * 1024;
constexpr size_t kPageFaultStackSize = 16 * 1024;
alignas(16) uint8_t g_df_stack[kDoubleFaultStackSize];
alignas(16) uint8_t g_pf_stack[kPageFaultStackSize];
Tss g_tss;
alignas(16) uint64_t g_gdt[5];

} // namespace

void gdt_init() {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFull;  // 64-bit kernel code: P, DPL0, S, exec/read, L, G
    g_gdt[2] = 0x00CF92000000FFFFull;  // kernel data: P, DPL0, S, read/write, D/B, G

    g_tss = Tss{};
    g_tss.ist[0] = reinterpret_cast<uint64_t>(g_df_stack + kDoubleFaultStackSize);  // IST1
    g_tss.ist[1] = reinterpret_cast<uint64_t>(g_pf_stack + kPageFaultStackSize);    // IST2
    g_tss.iomap_base = sizeof(Tss);  // no I/O permission bitmap

    uint64_t base = reinterpret_cast<uint64_t>(&g_tss);
    uint64_t limit = sizeof(Tss) - 1;
    g_gdt[3] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40)  // P, 64-bit TSS (available)
               | (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[4] = base >> 32;

    Gdtr gdtr{static_cast<uint16_t>(sizeof(g_gdt) - 1), reinterpret_cast<uint64_t>(g_gdt)};
    asm volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "ltr %w3\n\t"
        :
        : "m"(gdtr), "i"(static_cast<uint64_t>(kKernelCode)), "r"(static_cast<uint32_t>(kKernelData)),
          "r"(static_cast<uint32_t>(kTssSelector))
        : "rax", "memory");
}

} // namespace osef::bm
