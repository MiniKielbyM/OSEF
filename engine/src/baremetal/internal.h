#pragma once
// Declarations shared between the bare-metal layer's own translation units.
// Not for the platform glue (use arch.h).

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "arch.h"

struct limine_framebuffer;

#if defined(__GNUC__) || defined(__clang__)
#  define OSEF_BM_PRINTF(fmt_index, args_index) __attribute__((format(printf, fmt_index, args_index)))
#else
#  define OSEF_BM_PRINTF(fmt_index, args_index)
#endif

namespace osef::bm {

// ---- formatter (fmt.cpp) ---------------------------------------------------
// Tiny snprintf: %s %c %d %i %u %x %X %p %%, flags '0' '-', width, length
// modifiers l ll z. Returns the length that would have been written; always
// NUL-terminates when n > 0.
size_t vfmt(char* buf, size_t n, const char* fmt, va_list ap);
size_t fmt(char* buf, size_t n, const char* fmt, ...) OSEF_BM_PRINTF(3, 4);
// "[osef] <message>\n" on every console.
void log(const char* fmt, ...) OSEF_BM_PRINTF(1, 2);
size_t cstr_len(const char* s);
void copy_cstr(char* dst, size_t dst_size, const char* src);

// ---- interrupt frame (isr.S layout; lowest address first) -------------------
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;  // pushed by the CPU
};
static_assert(sizeof(InterruptFrame) == 22 * 8, "InterruptFrame must match isr.S");

// ---- CPU tables (gdt.cpp, idt.cpp) -----------------------------------------
constexpr uint16_t kKernelCode = 0x08;
constexpr uint16_t kKernelData = 0x10;
constexpr uint16_t kTssSelector = 0x18;
void gdt_init();  // GDT + TSS (IST1 = double-fault stack, IST2 = page-fault stack)
void idt_init();  // vectors 0-47 -> isr.S stubs
extern volatile uint32_t g_irq_depth;  // > 0 while an IRQ handler runs

// ---- exceptions / protection (protect.cpp, panic.cpp) ----------------------
const char* exception_name(uint64_t vector);
// Called by the IDT dispatcher for vectors 0-31. Returns only for benign
// exceptions (NMI); otherwise longjmps into protected_call or panics.
void handle_exception(InterruptFrame* frame);
[[noreturn]] void panic_exception(const InterruptFrame& frame);
void emergency_write(const char* s);  // serial-only, safe during a nested panic
bool panicking();                     // true once panic()/panic_exception() started
// True if `addr` is in the never-mapped guard below the kernel stack, i.e. a
// page fault there is a stack overflow.
bool in_stack_guard(uint64_t addr);

// ---- legacy PIC / PIT (pic.cpp, pit.cpp) -----------------------------------
void pic_init();  // remap to 32-47, all lines masked
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_eoi(uint8_t irq);
bool pic_is_spurious(uint8_t irq);  // handles EOI rules for spurious 7 / 15
void pit_init(uint32_t hz);
void pit_irq();

// ---- devices ---------------------------------------------------------------
bool serial_init(uint32_t baud);  // true if a UART answered the loopback test
bool serial_present();
void serial_enable_rx_irq();
void serial_irq();
void serial_write(const char* s, size_t n);
void serial_flush();  // wait (bounded) until the transmitter is completely idle
bool serial_getc(char& c);

bool keyboard_init();  // false if no i8042 controller
void keyboard_irq();
bool keyboard_getc(char& c);

bool fbcon_init(const limine_framebuffer* fb);  // false if unsupported format
bool fbcon_active();
void fbcon_set_default_colors(int fg, int bg);
void fbcon_set_scale(uint32_t scale);  // 1-4: 8x8 font scaled; clears the screen  // ANSI 0-15, -1 = unchanged
void fbcon_write(const char* s, size_t n);

// ---- memory / runtime ------------------------------------------------------
void heap_add_region(void* base, size_t size);  // call once per usable region
void run_constructors();

// ---- boot information (boot.cpp) -------------------------------------------
const limine_framebuffer* boot_framebuffer();  // null if none
bool console_serial_enabled();
bool console_fb_enabled();

// ---- single-producer (IRQ) / single-consumer ring buffer -------------------
template <uint32_t N>
struct Ring {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    volatile uint32_t head = 0;  // written by the producer only
    volatile uint32_t tail = 0;  // written by the consumer only
    char data[N] = {};

    bool push(char c) {
        uint32_t h = head;
        if (h - tail >= N) return false;  // full: drop
        data[h & (N - 1)] = c;
        asm volatile("" : : : "memory");
        head = h + 1;
        return true;
    }
    bool pop(char& c) {
        uint32_t t = tail;
        if (t == head) return false;
        c = data[t & (N - 1)];
        asm volatile("" : : : "memory");
        tail = t + 1;
        return true;
    }
};

} // namespace osef::bm
