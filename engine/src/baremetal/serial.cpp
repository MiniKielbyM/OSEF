// 16550 UART on COM1: polled transmit, interrupt-driven receive (IRQ4) into
// a ring buffer.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {
constexpr uint16_t kCom1 = 0x3F8;
constexpr uint16_t kData = kCom1 + 0, kIer = kCom1 + 1, kFcr = kCom1 + 2, kLcr = kCom1 + 3, kMcr = kCom1 + 4,
                   kLsr = kCom1 + 5;

bool g_present = false;
bool g_skip_lf = false;  // CR LF from a terminal counts as one Enter
Ring<1024> g_rx;

void rx_byte(uint8_t b) {
    char c = static_cast<char>(b);
    if (c == '\r') {
        g_skip_lf = true;
        c = '\n';
    } else if (c == '\n' && g_skip_lf) {
        g_skip_lf = false;
        return;
    } else {
        g_skip_lf = false;
        if (b == 0x7F) c = '\b';  // DEL (what most terminals send for Backspace)
    }
    g_rx.push(c);
}
} // namespace

bool serial_init(uint32_t baud) {
    if (baud == 0 || baud > 115200) baud = 115200;
    uint32_t divisor = 115200 / baud;
    cpu::outb(kIer, 0x00);                                   // no interrupts yet
    cpu::outb(kLcr, 0x80);                                   // DLAB on
    cpu::outb(kData, static_cast<uint8_t>(divisor & 0xFF));  // divisor low
    cpu::outb(kIer, static_cast<uint8_t>(divisor >> 8));     // divisor high
    cpu::outb(kLcr, 0x03);                                   // 8N1, DLAB off
    cpu::outb(kFcr, 0xC7);                                   // FIFO on, clear, 14-byte threshold
    cpu::outb(kMcr, 0x1E);                                   // loopback test
    cpu::outb(kData, 0xAE);
    if (cpu::inb(kData) != 0xAE) {
        g_present = false;
        cpu::outb(kMcr, 0x0F);
        return false;
    }
    cpu::outb(kMcr, 0x0F);  // normal: DTR, RTS, OUT1, OUT2 (OUT2 gates the IRQ line)
    g_present = true;
    return true;
}

bool serial_present() { return g_present; }

void serial_enable_rx_irq() {
    if (!g_present) return;
    while (cpu::inb(kLsr) & 0x01) (void)cpu::inb(kData);  // drop stale input
    cpu::outb(kIer, 0x01);                                  // received-data-available
    pic_unmask(4);
}

void serial_irq() {
    if (!g_present) return;
    for (int i = 0; i < 64 && (cpu::inb(kLsr) & 0x01); ++i) rx_byte(cpu::inb(kData));
}

void serial_write(const char* s, size_t n) {
    if (!g_present) return;
    for (size_t i = 0; i < n; ++i) {
        // Bounded wait so a wedged/absent UART cannot hang the kernel.
        for (uint32_t spin = 0; spin < 1000000 && !(cpu::inb(kLsr) & 0x20); ++spin) cpu::pause();
        cpu::outb(kData, static_cast<uint8_t>(s[i]));
    }
}

void serial_flush() {
    if (!g_present) return;
    // LSR bit 6 (TEMT): transmit FIFO and shift register both empty.
    for (uint32_t spin = 0; spin < 1000000 && !(cpu::inb(kLsr) & 0x40); ++spin) cpu::pause();
}

bool serial_getc(char& c) {
    if (!g_present) return false;
    // Also poll the UART here. The legacy PIC is edge-triggered: if IRQ4 is
    // masked, interrupts are off, or the IRQ handler ever leaves bytes in
    // the FIFO, the UART's interrupt line stays high, no new edge arrives and
    // interrupt-driven receive would stall for good. Polling with interrupts
    // off keeps rx_byte() single-producer (it is not re-entrant).
    const bool irqs_on = cpu::interrupts_enabled();
    cpu::cli();
    for (int i = 0; i < 64 && (cpu::inb(kLsr) & 0x01); ++i) rx_byte(cpu::inb(kData));
    if (irqs_on) cpu::sti();
    return g_rx.pop(c);
}

} // namespace osef::bm
