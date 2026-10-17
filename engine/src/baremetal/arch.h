#pragma once
// OSEF bare-metal x86_64 hardware layer: internal API for the platform glue.
//
// Everything here is freestanding (no standard library, no osef core). The
// kernel entry point is `osef_kmain` (boot.cpp); by the time the user's main()
// runs, every function below is usable.
//
// Threading model: single CPU, single thread. Interrupt handlers (PIT,
// keyboard, serial RX) only touch their own ring buffers / counters, so
// nothing here needs locking from the caller's side.

#include <stddef.h>
#include <stdint.h>

#include <osef/hardware.h>

namespace osef::bm {

struct HeapStats {
    uint64_t total;  // bytes managed by the kernel heap
    uint64_t used;   // bytes currently allocated (including block headers)
};

struct RtcTime {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
};

struct PciInfo {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, programming_interface;
};

// Console output: COM1 (if Hw::Serial was declared; bytes are sent raw, no
// "\n" -> "\r\n" translation, ANSI escapes pass through) and the framebuffer
// text console (if Hw::Framebuffer was declared and a framebuffer exists;
// it interprets "\r \n \b \t" and an ANSI subset: SGR 0/1/22/30-37/39/40-47/
// 49/90-97/100-107, ESC[2J, ESC[H / ESC[r;cH, ESC[K, ESC[nA/B/C/D).
void console_write(const char* s, size_t n);
void console_write(const char* s);  // NUL-terminated convenience overload
void console_set_scale(int scale);      // framebuffer font scale 1-4 (clears the screen)
void console_set_colors(int fg, int bg); // framebuffer default colors (ANSI 0-15, -1 = unchanged)

// Next decoded input character from the PS/2 keyboard and COM1 ring buffers
// (keyboard first). Enter / CR -> '\n', Backspace / DEL -> '\b', Ctrl+<letter>
// -> 1..26. Never blocks; returns false when both buffers are empty.
bool input_getc(char& c);
bool rtc_read(RtcTime& time);
size_t pci_scan(PciInfo* devices, size_t capacity);
void acpi_init();
bool acpi_power_off();
uint64_t hhdm_offset();
uintptr_t rsdp_address();
// Maps physical memory the HHDM does not cover into a kernel window and
// returns its virtual address. Mappings are permanent.
void* map_physical(uint64_t phys, uint64_t size, bool uncached);
void* map_mmio(uint64_t phys, uint64_t size);  // uncached (device registers)

// Milliseconds since the PIT was started (1000 Hz, IRQ0).
uint64_t ticks_ms();

// sti; hlt -- returns after the next interrupt (at the latest ~1 ms later).
void wait_for_interrupt();

// Kernel heap. heap_alloc never returns null (panics on out-of-memory);
// alignment is at least 16, larger powers of two are honoured.
// heap_free(nullptr) is a no-op; double free / foreign pointers panic.
void* heap_alloc(size_t size, size_t align);
void heap_free(void* p);
HeapStats heap_stats();

// Run fn(arg). If it raises a CPU exception (page fault, #GP, #UD, #DE, ...)
// or calls protected_fault(), unwind back here (setjmp/longjmp, no
// destructors run) and return false with a NUL-terminated message in msg,
// e.g. "page fault at 0x0000000000000000 (rip=0xffffffff80001234, error=0x2)".
// Nests. Interrupt state is restored on the fault path.
bool protected_call(void (*fn)(void*), void* arg, char* msg, size_t msg_size);
// Abort the innermost protected_call with `msg`. Outside protected_call it panics.
[[noreturn]] void protected_fault(const char* msg);

// Kernel panic: print the message on every console, then halt. (Also writes
// 0x7f to the QEMU isa-debug-exit port 0xf4, so test VMs exit with status 255.)
[[noreturn]] void panic(const char* msg);

// Print "System halted (exit code N)", then: write exit_code + 1 to port 0xf4
// (QEMU isa-debug-exit: QEMU exits with status ((exit_code + 1) << 1) | 1;
// codes outside 0..125 are reported as 125, and `osef-image run --test`
// decodes the status back to the exit code), try the QEMU/Bochs/VirtualBox
// ACPI poweroff ports, and finally cli; hlt forever.
[[noreturn]] void power_off(int exit_code);

// Firmware Limine booted through: "bios", "uefi" or "unknown".
const char* firmware_name();

// RDRAND if the CPU supports it, else TSC-seeded splitmix64.
uint64_t random_u64();

// The OSEF_HARDWARE(...) declaration linked into this kernel.
const osef::HardwareSpec& spec();

} // namespace osef::bm
