// PS/2 keyboard (i8042, scancode set 1 as delivered with controller
// translation), IRQ1 -> decoded ASCII ring buffer.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {
constexpr uint16_t kData = 0x60;
constexpr uint16_t kStatus = 0x64;  // read: status, write: command

// Scancode set 1, make codes 0x00-0x39.
constexpr char kNormal[0x3A] = {
    0,    0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q',  'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a',  's',
    'd',  'f',  'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c',  'v',
    'b',  'n',  'm', ',', '.', '/', 0,   '*', 0,   ' ',
};
constexpr char kShifted[0x3A] = {
    0,    0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q',  'W',  'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A',  'S',
    'D',  'F',  'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C',  'V',
    'B',  'N',  'M', '<', '>', '?', 0,   '*', 0,   ' ',
};

bool g_present = false;
bool g_shift_l = false, g_shift_r = false, g_ctrl = false, g_caps = false, g_extended = false;
Ring<256> g_keys;

bool wait_input_clear() {
    for (uint32_t i = 0; i < 100000; ++i) {
        if (!(cpu::inb(kStatus) & 0x02)) return true;
        cpu::pause();
    }
    return false;
}
bool wait_output_full() {
    for (uint32_t i = 0; i < 100000; ++i) {
        if (cpu::inb(kStatus) & 0x01) return true;
        cpu::pause();
    }
    return false;
}

void decode(uint8_t sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    bool release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;
    if (g_extended) {
        g_extended = false;
        if (code == 0x1D) g_ctrl = !release;                     // right Ctrl
        else if (code == 0x1C && !release) g_keys.push('\n');    // keypad Enter
        else if (code == 0x35 && !release) g_keys.push('/');     // keypad /
        return;  // arrows etc. are not mapped
    }
    switch (code) {
    case 0x2A: g_shift_l = !release; return;
    case 0x36: g_shift_r = !release; return;
    case 0x1D: g_ctrl = !release; return;
    case 0x3A:
        if (!release) g_caps = !g_caps;
        return;
    default: break;
    }
    if (release || code >= sizeof(kNormal)) return;
    bool shift = g_shift_l || g_shift_r;
    char c = shift ? kShifted[code] : kNormal[code];
    if (c == 0) return;
    bool letter = (kNormal[code] >= 'a' && kNormal[code] <= 'z');
    if (letter && g_caps) c = shift ? kNormal[code] : kShifted[code];
    if (g_ctrl && letter) c = static_cast<char>(kNormal[code] - 'a' + 1);
    g_keys.push(c);
}

void drain() {
    for (int i = 0; i < 64 && (cpu::inb(kStatus) & 0x01); ++i) {
        (void)cpu::inb(kData);
        cpu::io_wait();
    }
}
} // namespace

bool keyboard_init() {
    if (cpu::inb(kStatus) == 0xFF) return false;  // no controller (floating bus)
    // Disable both ports while reconfiguring, so a keystroke (or mouse byte)
    // cannot be read back as the configuration byte below.
    if (wait_input_clear()) cpu::outb(kStatus, 0xAD);  // disable port 1 (keyboard)
    if (wait_input_clear()) cpu::outb(kStatus, 0xA7);  // disable port 2 (mouse); ignored if absent
    drain();
    // Controller configuration: port-1 IRQ on, port-1 clock on, set-1
    // translation; port-2 IRQ off and its clock off. IRQ12 is never unmasked,
    // so a streaming mouse would otherwise fill the shared output buffer and
    // block keyboard bytes (IRQ1 is not raised for aux data).
    if (wait_input_clear()) {
        cpu::outb(kStatus, 0x20);
        if (wait_output_full()) {
            uint8_t cfg = cpu::inb(kData);
            cfg = static_cast<uint8_t>((cfg | 0x01 | 0x20 | 0x40) & ~(0x02 | 0x10));
            if (wait_input_clear()) {
                cpu::outb(kStatus, 0x60);
                if (wait_input_clear()) cpu::outb(kData, cfg);
            }
        }
    }
    if (wait_input_clear()) cpu::outb(kStatus, 0xAE);  // enable port 1
    drain();
    g_present = true;
    pic_unmask(1);
    return true;
}

void keyboard_irq() {
    // Mouse bytes (status bit 5) are drained but ignored.
    for (int i = 0; i < 16; ++i) {
        uint8_t st = cpu::inb(kStatus);
        if (!(st & 0x01)) break;
        uint8_t b = cpu::inb(kData);
        if (!(st & 0x20)) decode(b);
    }
}

bool keyboard_getc(char& c) {
    if (!g_present) return false;
    return g_keys.pop(c);
}

} // namespace osef::bm
