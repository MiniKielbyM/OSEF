// Minimal virtual memory manager: maps physical ranges that Limine's HHDM
// leaves out (device MMIO such as USB controllers, ACPI tables in reserved
// memory) into a dedicated kernel window, using the page tables Limine set up.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {

constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kWritable = 1ull << 1;
constexpr uint64_t kWriteThrough = 1ull << 3;
constexpr uint64_t kCacheDisable = 1ull << 4;
constexpr uint64_t kHuge = 1ull << 7;
constexpr uint64_t kAddrMask = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kPage = 4096;

// PML4 slot 384: unused by Limine (HHDM is slot 256+, the kernel slot 511).
constexpr uint64_t kWindowBase = 0xFFFFC00000000000ull;
constexpr uint64_t kWindowSize = 512ull << 30;  // one PML4 slot's worth is plenty
uint64_t g_next = kWindowBase;

uint64_t* table_at(uint64_t phys) { return reinterpret_cast<uint64_t*>(phys + hhdm_offset()); }

// The next-level table for entry `e`, allocating a zeroed one if missing.
uint64_t* next_table(uint64_t& e) {
    if (!(e & kPresent)) {
        auto* page = static_cast<uint64_t*>(heap_alloc(kPage, kPage));
        for (size_t i = 0; i < kPage / 8; ++i) page[i] = 0;
        const uint64_t phys = reinterpret_cast<uint64_t>(page) - hhdm_offset();
        e = phys | kPresent | kWritable;
    }
    if (e & kHuge) panic("vmm: unexpected huge page in the MMIO window");
    return table_at(e & kAddrMask);
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* pml4 = table_at(cpu::read_cr3() & kAddrMask);
    uint64_t* pdpt = next_table(pml4[(virt >> 39) & 511]);
    uint64_t* pd = next_table(pdpt[(virt >> 30) & 511]);
    uint64_t* pt = next_table(pd[(virt >> 21) & 511]);
    pt[(virt >> 12) & 511] = (phys & kAddrMask) | flags;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

} // namespace

void* map_physical(uint64_t phys, uint64_t size, bool uncached) {
    if (size == 0) size = 1;
    const uint64_t first = phys & ~(kPage - 1);
    const uint64_t last = (phys + size - 1) & ~(kPage - 1);
    const uint64_t pages = (last - first) / kPage + 1;
    if (g_next + pages * kPage > kWindowBase + kWindowSize) panic("vmm: MMIO window exhausted");
    const uint64_t virt = g_next;
    g_next += pages * kPage;
    const uint64_t flags = kPresent | kWritable | (uncached ? (kCacheDisable | kWriteThrough) : 0);
    for (uint64_t i = 0; i < pages; ++i) map_page(virt + i * kPage, first + i * kPage, flags);
    return reinterpret_cast<void*>(virt + (phys - first));
}

void* map_mmio(uint64_t phys, uint64_t size) { return map_physical(phys, size, true); }

} // namespace osef::bm
