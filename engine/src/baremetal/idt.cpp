// IDT: CPU exceptions 0-31 and remapped legacy-PIC IRQs 32-47, all routed
// through the isr.S stubs into osef_bm_isr_dispatch().

#include "arch.h"
#include "cpu.h"
#include "internal.h"

extern "C" const uint64_t osef_bm_isr_stubs[256];
extern "C" const uint64_t osef_bm_int_thunks[256];
extern "C" void osef_bm_isr_dispatch(osef::bm::InterruptFrame* frame);

namespace osef::bm {

volatile uint32_t g_irq_depth = 0;

namespace {

struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};
static_assert(sizeof(IdtEntry) == 16, "IDT entry layout");

struct __attribute__((packed)) Idtr {
    uint16_t limit;
    uint64_t base;
};

alignas(16) IdtEntry g_idt[256];

void set_gate(uint8_t vector, uint64_t handler, uint8_t ist) {
    IdtEntry& e = g_idt[vector];
    e.offset_low = static_cast<uint16_t>(handler & 0xFFFF);
    e.selector = kKernelCode;
    e.ist = ist;
    e.type_attr = 0x8E;  // present, DPL0, 64-bit interrupt gate (IF cleared on entry)
    e.offset_mid = static_cast<uint16_t>((handler >> 16) & 0xFFFF);
    e.offset_high = static_cast<uint32_t>(handler >> 32);
    e.reserved = 0;
}

constexpr uint8_t kDoubleFaultIst = 1;
constexpr uint8_t kPageFaultIst = 2;  // see gdt.cpp

} // namespace

void idt_init() {
    for (auto& e : g_idt) e = IdtEntry{};
    // Every vector gets a real stub; vectors without a handler are ignored in
    // the dispatcher (e.g. a stray local-APIC spurious interrupt).
    for (uint32_t v = 0; v < 256; ++v)
        set_gate(static_cast<uint8_t>(v), osef_bm_isr_stubs[v], v == 8 ? kDoubleFaultIst : v == 14 ? kPageFaultIst : 0);
    Idtr idtr{static_cast<uint16_t>(sizeof(g_idt) - 1), reinterpret_cast<uint64_t>(g_idt)};
    asm volatile("lidt %0" : : "m"(idtr) : "memory");
}

} // namespace osef::bm

namespace osef::bm {
namespace {

// Custom interrupts: per-vector "claimed" flag, an optional immediate handler
// (interrupt context) and a pending counter the kernel drains every tick.
struct Custom {
    bool claimed = false;
    InterruptHandler handler = nullptr;
    void* user = nullptr;
};
Custom g_custom[256];
volatile uint32_t g_pending[256];

// IRQ lines the engine's own drivers use (always unmasked when enabled).
bool engine_irq(uint8_t irq) { return irq == 0 || irq == 1 || irq == 2 || irq == 4; }

struct IrqGuard {
    bool was_on = cpu::interrupts_enabled();
    IrqGuard() { cpu::cli(); }
    ~IrqGuard() {
        if (was_on) cpu::sti();
    }
};

} // namespace

bool claim_interrupt(uint8_t vector, InterruptHandler handler, void* user) {
    if (vector < 32) return false;  // CPU exceptions are reserved
    IrqGuard guard;
    g_custom[vector] = Custom{true, handler, user};
    g_pending[vector] = 0;
    if (vector < 48) {
        const uint8_t irq = static_cast<uint8_t>(vector - 32);
        if (irq >= 8) pic_unmask(2);  // cascade
        pic_unmask(irq);
    }
    return true;
}

void release_interrupt(uint8_t vector) {
    if (vector < 32) return;
    IrqGuard guard;
    g_custom[vector] = Custom{};
    g_pending[vector] = 0;
    if (vector < 48 && !engine_irq(static_cast<uint8_t>(vector - 32))) pic_mask(static_cast<uint8_t>(vector - 32));
}

uint32_t take_interrupts(uint8_t vector) {
    IrqGuard guard;
    const uint32_t n = g_pending[vector];
    g_pending[vector] = 0;
    return n;
}

bool raise_interrupt(uint8_t vector) {
    // Only OS vectors: raising 32-47 would fake a PIC interrupt and send a
    // bogus EOI; 0-31 are CPU exceptions.
    if (vector < 48 || !osef_bm_int_thunks[vector]) return false;
    reinterpret_cast<void (*)()>(osef_bm_int_thunks[vector])();
    return true;
}

} // namespace osef::bm

extern "C" void osef_bm_isr_dispatch(osef::bm::InterruptFrame* frame) {
    using namespace osef::bm;
    const uint64_t vector = frame->vector;
    if (vector < 32) {
        handle_exception(frame);
        return;
    }
    Custom& custom = g_custom[vector];
    if (vector >= 48) {
        // OS vector (software or device interrupt); unclaimed ones are stray.
        if (!custom.claimed) return;
        g_irq_depth = g_irq_depth + 1;
        g_pending[vector] = g_pending[vector] + 1;
        if (custom.handler) custom.handler(static_cast<uint8_t>(vector), custom.user);
        g_irq_depth = g_irq_depth - 1;
        return;
    }
    uint8_t irq = static_cast<uint8_t>(vector - 32);
    if (pic_is_spurious(irq)) return;
    g_irq_depth = g_irq_depth + 1;
    switch (irq) {
    case 0: pit_irq(); break;
    case 1: keyboard_irq(); break;
    case 4: serial_irq(); break;
    default: break;
    }
    if (custom.claimed) {
        g_pending[vector] = g_pending[vector] + 1;
        if (custom.handler) custom.handler(static_cast<uint8_t>(vector), custom.user);
    }
    g_irq_depth = g_irq_depth - 1;
    pic_eoi(irq);
}
