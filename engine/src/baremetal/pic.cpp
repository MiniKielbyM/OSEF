// Legacy 8259A PIC pair, remapped to vectors 32-47.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {
constexpr uint16_t kPic1Cmd = 0x20, kPic1Data = 0x21;
constexpr uint16_t kPic2Cmd = 0xA0, kPic2Data = 0xA1;
constexpr uint8_t kEoi = 0x20;
constexpr uint8_t kReadIsr = 0x0B;

uint8_t g_mask1 = 0xFF, g_mask2 = 0xFF;

void write_masks() {
    cpu::outb(kPic1Data, g_mask1);
    cpu::outb(kPic2Data, g_mask2);
}
} // namespace

void pic_init() {
    cpu::outb(kPic1Cmd, 0x11);  // ICW1: init, ICW4 follows
    cpu::io_wait();
    cpu::outb(kPic2Cmd, 0x11);
    cpu::io_wait();
    cpu::outb(kPic1Data, 32);   // ICW2: master vector base
    cpu::io_wait();
    cpu::outb(kPic2Data, 40);   // ICW2: slave vector base
    cpu::io_wait();
    cpu::outb(kPic1Data, 4);    // ICW3: slave on IRQ2
    cpu::io_wait();
    cpu::outb(kPic2Data, 2);    // ICW3: cascade identity
    cpu::io_wait();
    cpu::outb(kPic1Data, 0x01); // ICW4: 8086 mode
    cpu::io_wait();
    cpu::outb(kPic2Data, 0x01);
    cpu::io_wait();
    g_mask1 = 0xFF & ~(1u << 2);  // keep the cascade line open
    g_mask2 = 0xFF;
    write_masks();
}

void pic_unmask(uint8_t irq) {
    if (irq < 8) g_mask1 = static_cast<uint8_t>(g_mask1 & ~(1u << irq));
    else g_mask2 = static_cast<uint8_t>(g_mask2 & ~(1u << (irq - 8)));
    write_masks();
}

void pic_mask(uint8_t irq) {
    if (irq < 8) g_mask1 = static_cast<uint8_t>(g_mask1 | (1u << irq));
    else g_mask2 = static_cast<uint8_t>(g_mask2 | (1u << (irq - 8)));
    write_masks();
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) cpu::outb(kPic2Cmd, kEoi);
    cpu::outb(kPic1Cmd, kEoi);
}

bool pic_is_spurious(uint8_t irq) {
    if (irq == 7) {
        cpu::outb(kPic1Cmd, kReadIsr);
        if ((cpu::inb(kPic1Cmd) & 0x80) == 0) return true;  // spurious: no EOI
    } else if (irq == 15) {
        cpu::outb(kPic2Cmd, kReadIsr);
        if ((cpu::inb(kPic2Cmd) & 0x80) == 0) {
            cpu::outb(kPic1Cmd, kEoi);  // the master did see the cascade IRQ
            return true;
        }
    }
    return false;
}

} // namespace osef::bm
