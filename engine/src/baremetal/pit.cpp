// 8253/8254 PIT channel 0 as the system tick (IRQ0).

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {
constexpr uint32_t kPitHz = 1193182;
volatile uint64_t g_nanos = 0;
uint64_t g_nanos_per_tick = 1000000;
} // namespace

void pit_init(uint32_t hz) {
    uint32_t divisor = (kPitHz + hz / 2) / hz;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;
    // Exact tick length, so ticks_ms() does not drift (1193 counts = 999847 ns).
    g_nanos_per_tick = (static_cast<uint64_t>(divisor) * 1000000000ull + kPitHz / 2) / kPitHz;
    cpu::outb(0x43, 0x34);  // channel 0, lobyte/hibyte, mode 2 (rate generator), binary
    cpu::outb(0x40, static_cast<uint8_t>(divisor & 0xFF));
    cpu::outb(0x40, static_cast<uint8_t>(divisor >> 8));
    pic_unmask(0);
}

void pit_irq() { g_nanos = g_nanos + g_nanos_per_tick; }

uint64_t ticks_ms() { return g_nanos / 1000000ull; }

void wait_for_interrupt() { cpu::sti_hlt(); }

} // namespace osef::bm
