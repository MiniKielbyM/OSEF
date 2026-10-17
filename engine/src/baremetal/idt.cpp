// IDT: CPU exceptions 0-31 and remapped legacy-PIC IRQs 32-47, all routed
// through the isr.S stubs into osef_bm_isr_dispatch().

#include "cpu.h"
#include "internal.h"

extern "C" const uint64_t osef_bm_isr_stubs[48];
extern "C" void osef_bm_isr_ignore();
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
    for (uint8_t v = 0; v < 48; ++v)
        set_gate(v, osef_bm_isr_stubs[v], v == 8 ? kDoubleFaultIst : v == 14 ? kPageFaultIst : 0);
    // Stray / spurious vectors 48-255 (see isr.S): plain iretq.
    for (uint32_t v = 48; v < 256; ++v)
        set_gate(static_cast<uint8_t>(v), reinterpret_cast<uint64_t>(&osef_bm_isr_ignore), 0);
    Idtr idtr{static_cast<uint16_t>(sizeof(g_idt) - 1), reinterpret_cast<uint64_t>(g_idt)};
    asm volatile("lidt %0" : : "m"(idtr) : "memory");
}

} // namespace osef::bm

extern "C" void osef_bm_isr_dispatch(osef::bm::InterruptFrame* frame) {
    using namespace osef::bm;
    uint64_t vector = frame->vector;
    if (vector < 32) {
        handle_exception(frame);
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
    g_irq_depth = g_irq_depth - 1;
    pic_eoi(irq);
}
