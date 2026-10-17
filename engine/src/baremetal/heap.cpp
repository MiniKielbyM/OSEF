// Kernel heap: one or more physical regions (via the HHDM), address-ordered free list, first fit,
// block splitting, coalescing with both neighbours on free, 16-byte minimum
// alignment (larger powers of two honoured). Every block carries a 16-byte
// header whose tag encodes state + size, so double frees, frees of foreign
// pointers and most header overwrites are caught and panic.

#include "internal.h"

namespace osef::bm {

namespace {

constexpr uint64_t kTagUsed = 0x05EFA110CA7EDB10ull;
constexpr uint64_t kTagFree = 0x05EFF4EEB10C4B1Dull;
constexpr size_t kAlign = 16;
constexpr size_t kHeader = 16;
constexpr size_t kMinBlock = 32;  // header + room for the free-list link

struct Block {
    uint64_t size;  // whole block in bytes, including this header; multiple of 16
    uint64_t tag;   // kTagUsed/kTagFree ^ size
    // free blocks only (overlays the payload):
    Block* next;
};

Block* g_free = nullptr;  // sorted by address
uintptr_t g_begin = 0, g_end = 0;
uint64_t g_used = 0;
uint64_t g_total = 0;  // sum of all regions

inline uintptr_t align_up(uintptr_t v, size_t a) { return (v + a - 1) & ~(static_cast<uintptr_t>(a) - 1); }
inline uintptr_t addr(const Block* b) { return reinterpret_cast<uintptr_t>(b); }
inline Block* at(uintptr_t a) { return reinterpret_cast<Block*>(a); }

void make_free(Block* b, uint64_t size, Block* next) {
    b->size = size;
    b->tag = kTagFree ^ size;
    b->next = next;
}
void make_used(Block* b, uint64_t size) {
    b->size = size;
    b->tag = kTagUsed ^ size;
}

[[noreturn]] void heap_panic(const char* what, const void* p) {
    char msg[160];
    fmt(msg, sizeof msg, "kernel heap: %s (pointer %p)", what, p);
    panic(msg);
}

void check_free_block(const Block* b) {
    if (addr(b) < g_begin || addr(b) + kMinBlock > g_end || (addr(b) & (kAlign - 1)) || b->tag != (kTagFree ^ b->size))
        heap_panic("free list corrupted (heap overflow by a previous allocation?)", b);
}

} // namespace

void heap_add_region(void* base, size_t size) {
    uintptr_t begin = align_up(reinterpret_cast<uintptr_t>(base), kAlign);
    uintptr_t end = (reinterpret_cast<uintptr_t>(base) + size) & ~(static_cast<uintptr_t>(kAlign) - 1);
    if (end <= begin || end - begin < kMinBlock) return;
    if (g_begin == 0 || begin < g_begin) g_begin = begin;
    if (end > g_end) g_end = end;
    g_total += end - begin;
    // Insert in address order (regions never overlap).
    Block* prev = nullptr;
    Block* cur = g_free;
    while (cur && addr(cur) < begin) {
        prev = cur;
        cur = cur->next;
    }
    Block* b = at(begin);
    make_free(b, end - begin, cur);
    if (prev) prev->next = b;
    else g_free = b;
}

void* heap_alloc(size_t size, size_t align) {
    if (g_free == nullptr && g_total == 0) panic("kernel heap: allocation before the heap was set up");
    if (align < kAlign) align = kAlign;
    if (align & (align - 1)) heap_panic("alignment is not a power of two", reinterpret_cast<void*>(align));
    if (size == 0) size = 1;
    uint64_t capacity = g_total;
    if (size > capacity || align > capacity) {
        char msg[160];
        fmt(msg, sizeof msg, "out of memory: request of %zu bytes exceeds the %llu byte heap", size,
            static_cast<unsigned long long>(capacity));
        panic(msg);
    }
    uint64_t need = align_up(size, kAlign) + kHeader;
    if (need < kMinBlock) need = kMinBlock;

    Block* prev = nullptr;
    for (Block* b = g_free; b; prev = b, b = b->next) {
        check_free_block(b);
        uintptr_t start = addr(b);
        uintptr_t end = start + b->size;
        uintptr_t payload = align_up(start + kHeader, align);
        // A leading gap must be big enough to stay behind as its own free block.
        while (payload - kHeader != start && payload - kHeader - start < kMinBlock) payload += align;
        uintptr_t blk = payload - kHeader;
        if (blk + need > end || blk + need < blk) continue;

        Block* next = b->next;
        // Leading fragment stays in the list in place of b (or b disappears).
        Block* link_prev = prev;
        if (blk != start) {
            make_free(b, blk - start, next);
            link_prev = b;
        } else {
            if (prev) prev->next = next;
            else g_free = next;
        }
        // Trailing fragment.
        uint64_t tail = end - (blk + need);
        uint64_t used_size = need;
        if (tail >= kMinBlock) {
            Block* t = at(blk + need);
            make_free(t, tail, next);
            if (link_prev) link_prev->next = t;
            else g_free = t;
        } else {
            used_size += tail;  // too small to split: hand it out with the block
            if (link_prev) link_prev->next = next;
        }
        Block* u = at(blk);
        make_used(u, used_size);
        g_used += used_size;
        return reinterpret_cast<void*>(payload);
    }

    char msg[200];
    fmt(msg, sizeof msg, "out of memory: cannot allocate %zu bytes (heap %llu KiB, %llu KiB in use)", size,
        static_cast<unsigned long long>(capacity / 1024), static_cast<unsigned long long>(g_used / 1024));
    panic(msg);
}

void heap_free(void* p) {
    if (!p) return;
    uintptr_t payload = reinterpret_cast<uintptr_t>(p);
    if (payload < g_begin + kHeader || payload >= g_end || (payload & (kAlign - 1)))
        heap_panic("free of a pointer that was not allocated by the heap", p);
    Block* b = at(payload - kHeader);
    if (b->tag == (kTagFree ^ b->size)) heap_panic("double free", p);
    if (b->tag != (kTagUsed ^ b->size) || b->size < kMinBlock || addr(b) + b->size > g_end)
        heap_panic("free of an invalid or corrupted block", p);
    uint64_t size = b->size;
    g_used -= size;

    // Find the insertion point in the address-ordered list.
    Block* prev = nullptr;
    Block* cur = g_free;
    while (cur && addr(cur) < addr(b)) {
        check_free_block(cur);
        prev = cur;
        cur = cur->next;
    }
    if (cur == b || (prev && addr(prev) + prev->size > addr(b))) heap_panic("double free (overlapping block)", p);

    make_free(b, size, cur);
    if (prev) prev->next = b;
    else g_free = b;
    // Coalesce with the following block.
    if (cur && addr(b) + b->size == addr(cur)) {
        check_free_block(cur);
        make_free(b, b->size + cur->size, cur->next);
        cur->tag = 0;
    }
    // Coalesce with the preceding block.
    if (prev && addr(prev) + prev->size == addr(b)) {
        make_free(prev, prev->size + b->size, b->next);
        b->tag = 0;
    }
}

HeapStats heap_stats() { return HeapStats{g_total, g_used}; }

} // namespace osef::bm
