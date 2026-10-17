// Kernel entry (osef_kmain), Limine requests and the boot sequence.
//
// Boot sequence (see .toolchain/CONTRACT.md, "Bare-metal architecture"):
//   0. enable SSE before any compiler-generated code can use it, then switch
//      to the kernel stack (linker.ld: 256 KiB above an unmapped guard)
//   1. check the Limine base revision        6. run .init_array constructors
//   2. hardware declaration + early COM1     7. framebuffer console
//   3. GDT (+TSS/IST) and IDT                8. PIC, PIT 1000 Hz, keyboard / COM1 IRQs, sti
//   4. (SSE already on)                      9. report the firmware we were booted with
//   5. heap from the usable memory regions   10. user main()  11. power off with its result

#include "cpu.h"
#include "internal.h"
#include "limine.h"

// The user's `int main()`. Never call `main` directly (ill-formed C++); bind
// to its symbol instead. Under -ffreestanding (__STDC_HOSTED__ == 0) clang
// and GCC treat main as an ordinary C++ function and mangle it as _Z4mainv;
// hosted compilers emit plain "main".
#if __STDC_HOSTED__
extern "C++" int osef_user_main() __asm__("main");
#else
extern "C++" int osef_user_main() __asm__("_Z4mainv");
#endif

namespace {

#define OSEF_LIMINE_REQUEST __attribute__((used, section(".limine_requests"), aligned(8)))

__attribute__((used, section(".limine_requests_start"), aligned(8)))
volatile uint64_t g_limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

OSEF_LIMINE_REQUEST volatile uint64_t g_limine_base_revision[] = LIMINE_BASE_REVISION(3);

OSEF_LIMINE_REQUEST volatile limine_memmap_request g_memmap_request = {
    LIMINE_MEMMAP_REQUEST_ID, 0, nullptr};
OSEF_LIMINE_REQUEST volatile limine_hhdm_request g_hhdm_request = {
    LIMINE_HHDM_REQUEST_ID, 0, nullptr};
OSEF_LIMINE_REQUEST volatile limine_rsdp_request g_rsdp_request = {
    LIMINE_RSDP_REQUEST_ID, 0, nullptr};
OSEF_LIMINE_REQUEST volatile limine_framebuffer_request g_framebuffer_request = {
    LIMINE_FRAMEBUFFER_REQUEST_ID, 0, nullptr};
OSEF_LIMINE_REQUEST volatile limine_firmware_type_request g_firmware_request = {
    LIMINE_FIRMWARE_TYPE_REQUEST_ID, 0, nullptr};
OSEF_LIMINE_REQUEST volatile limine_stack_size_request g_stack_request = {
    LIMINE_STACK_SIZE_REQUEST_ID, 0, nullptr, 256 * 1024};
OSEF_LIMINE_REQUEST volatile limine_bootloader_info_request g_bootloader_request = {
    LIMINE_BOOTLOADER_INFO_REQUEST_ID, 0, nullptr};

__attribute__((used, section(".limine_requests_end"), aligned(8)))
volatile uint64_t g_limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

bool g_serial_console = false;
bool g_fb_console = false;
bool g_has_rdrand = false;
uint64_t g_rng_state = 0;

constexpr uint64_t kMiB = 1024ull * 1024ull;

const char* firmware_label(uint32_t fw) {
    return fw == static_cast<uint32_t>(osef::Firmware::BIOS)   ? "BIOS"
           : fw == static_cast<uint32_t>(osef::Firmware::UEFI) ? "UEFI"
                                                                : "unspecified";
}

} // namespace

namespace osef::bm {

const osef::HardwareSpec& spec() { return osef_hardware_declaration; }

uint64_t hhdm_offset() {
    const limine_hhdm_response* r = g_hhdm_request.response;
    return r ? r->offset : 0;
}

uintptr_t rsdp_address() {
    const limine_rsdp_response* r = g_rsdp_request.response;
    return r && r->address ? reinterpret_cast<uintptr_t>(r->address) : 0;
}

const limine_framebuffer* boot_framebuffer() {
    limine_framebuffer_response* r = g_framebuffer_request.response;
    if (!r || r->framebuffer_count == 0 || !r->framebuffers) return nullptr;
    return r->framebuffers[0];
}

bool console_serial_enabled() { return g_serial_console; }
bool console_fb_enabled() { return g_fb_console; }

void console_set_scale(int scale) {
    if (fbcon_active() && scale >= 1) fbcon_set_scale(static_cast<uint32_t>(scale));
}

void console_set_colors(int fg, int bg) {
    if (fbcon_active()) fbcon_set_default_colors(fg, bg);
}

void console_write(const char* s, size_t n) {
    if (!s || n == 0) return;
    if (g_serial_console) serial_write(s, n);
    if (fbcon_active()) fbcon_write(s, n);
}

void console_write(const char* s) { console_write(s, cstr_len(s)); }

bool input_getc(char& c) {
    return keyboard_getc(c) || ((spec().devices & Hw::USB) && usb_getc(c)) || serial_getc(c);
}

const char* firmware_name() {
    limine_firmware_type_response* r = g_firmware_request.response;
    if (!r) return "unknown";
    switch (r->firmware_type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "bios";
    case LIMINE_FIRMWARE_TYPE_EFI32:
    case LIMINE_FIRMWARE_TYPE_EFI64: return "uefi";
    default: return "unknown";
    }
}

uint64_t random_u64() {
    if (g_has_rdrand) {
        for (int i = 0; i < 16; ++i) {
            uint64_t v;
            uint8_t ok;
            asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
            // Some AMD CPUs (family 15h/16h after resume, early Zen 2
            // microcode) report success but always return all-ones.
            if (ok && v != ~0ull) return v;
        }
    }
    // splitmix64 over a state that is re-stirred with the TSC on every call.
    g_rng_state += 0x9E3779B97F4A7C15ull ^ cpu::rdtsc();
    uint64_t z = g_rng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

namespace {

void setup_heap(const HardwareSpec& hw) {
    limine_memmap_response* mm = g_memmap_request.response;
    limine_hhdm_response* hhdm = g_hhdm_request.response;
    if (!mm || !hhdm) panic("bootloader did not provide a memory map / HHDM");

    uint64_t usable = 0, ram = 0;
    limine_memmap_entry* best = nullptr;
    for (uint64_t i = 0; i < mm->entry_count; ++i) {
        limine_memmap_entry* e = mm->entries[i];
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:
            usable += e->length;
            ram += e->length;
            if (!best || e->length > best->length) best = e;
            break;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE: ram += e->length; break;
        default: break;
        }
    }
    // Firmware always keeps a little RAM for itself (legacy area below 1 MiB,
    // ACPI NVS, UEFI runtime services), so a VM started with exactly
    // memory_mb MiB reports slightly less. Allow max(8 MiB, 1/8) for that.
    const uint64_t required = static_cast<uint64_t>(hw.memory_mb) * kMiB;
    uint64_t slack = required / 8;
    if (slack < 8 * kMiB) slack = 8 * kMiB;
    if (!best || ram + slack < required) {
        char msg[320];
        fmt(msg, sizeof msg,
            "not enough memory: this OS declares .memory_mb(%u) in OSEF_HARDWARE, but the machine only has "
            "%llu MiB of RAM (%llu MiB usable). Give the machine more memory (e.g. QEMU -m %u) or lower "
            ".memory_mb().",
            hw.memory_mb, static_cast<unsigned long long>(ram / kMiB), static_cast<unsigned long long>(usable / kMiB),
            hw.memory_mb);
        panic(msg);
    }
    // Every usable region becomes part of the heap (UEFI firmware leaves RAM
    // fragmented; the largest region alone can be well under half of it).
    uint32_t regions = 0;
    for (uint64_t i = 0; i < mm->entry_count; ++i) {
        limine_memmap_entry* e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE || e->length < 64 * 1024) continue;
        heap_add_region(reinterpret_cast<void*>(e->base + hhdm->offset), e->length);
        ++regions;
    }
    HeapStats hs = heap_stats();
    log("memory: %llu MiB RAM, %llu MiB usable; heap %llu MiB in %u region(s), largest %llu MiB",
        static_cast<unsigned long long>(ram / kMiB), static_cast<unsigned long long>(usable / kMiB),
        static_cast<unsigned long long>(hs.total / kMiB), regions,
        static_cast<unsigned long long>(best->length / kMiB));
}

[[noreturn]] __attribute__((noinline)) void kmain_body() {
    // 1. Limine base revision.
    if (!LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision)) cpu::halt_forever();

    // 2. Hardware declaration, early serial.
    const HardwareSpec& hw = spec();
    const bool spec_ok = hw.magic == kHardwareMagic && hw.version == kHardwareVersion && hw.size == sizeof(HardwareSpec);
    if (!spec_ok || (hw.devices & Hw::Serial)) {
        g_serial_console = serial_init(spec_ok ? hw.serial_baud : 115200);
    }
    if (!spec_ok) panic("invalid OSEF_HARDWARE declaration in the kernel image (magic/version/size mismatch)");
    g_fb_console = (hw.devices & Hw::Framebuffer) && boot_framebuffer() != nullptr;
    char name[kHardwareNameMax];
    copy_cstr(name, sizeof name, hw.name);
    log("OSEF bare-metal x86_64 kernel starting: %s", name);

    // 3. CPU tables.
    gdt_init();
    idt_init();
    // 4. SSE was enabled in osef_kmain (fxsave/fxrstor in the ISR stubs rely on it).
    g_has_rdrand = (cpu::cpuid(1).ecx & (1u << 30)) != 0;
    g_rng_state = cpu::rdtsc();

    // 5. Heap.
    setup_heap(hw);

    // 6. Global constructors (they may allocate).
    run_constructors();
    acpi_init();

    // 7. Framebuffer console.
    if (hw.devices & Hw::Framebuffer) {
        const limine_framebuffer* fb = boot_framebuffer();
        if (!fb) {
            log("warning: Hw::Framebuffer declared but the bootloader provided no framebuffer");
        } else if (!fbcon_init(fb)) {
            log("warning: unsupported framebuffer format (%llux%llu, %u bpp); framebuffer console disabled",
                static_cast<unsigned long long>(fb->width), static_cast<unsigned long long>(fb->height),
                static_cast<unsigned>(fb->bpp));
            g_fb_console = false;
        } else {
            log("framebuffer console: %llux%llu, %u bpp", static_cast<unsigned long long>(fb->width),
                static_cast<unsigned long long>(fb->height), static_cast<unsigned>(fb->bpp));
        }
    }

    // 8. Interrupt controllers, timer, input devices.
    pic_init();
    pit_init(1000);
    if (hw.devices & Hw::PS2Keyboard) {
        if (!keyboard_init()) log("warning: Hw::PS2Keyboard declared but no PS/2 controller was found");
    }
    if (hw.devices & Hw::RTC) {
        RtcTime time{};
        if (rtc_read(time))
            log("real-time clock: %04u-%02u-%02u %02u:%02u:%02u", time.year, time.month, time.day,
                time.hour, time.minute, time.second);
        else
            log("warning: Hw::RTC declared but the CMOS clock could not be read");
    }
    if (hw.devices & Hw::PCI) {
        PciInfo found[256];
        const size_t count = pci_scan(found, sizeof(found) / sizeof(found[0]));
        log("PCI: enumerated %u function(s)%s", static_cast<unsigned>(count),
            count > sizeof(found) / sizeof(found[0]) ? " (first 256 retained)" : "");
    }
    if ((hw.devices & Hw::Serial) && serial_present()) serial_enable_rx_irq();
    if ((hw.devices & Hw::Serial) && !serial_present()) log("warning: Hw::Serial declared but COM1 did not respond");
    cpu::sti();
    // USB needs the timer running (its controller timeouts use ticks_ms).
    if (hw.devices & Hw::USB) usb_init();

    // 9. Firmware check.
    const char* booted = firmware_name();
    const char* booted_label = booted[0] == 'b' ? "BIOS" : booted[0] == 'u' ? "UEFI" : "unknown firmware";
    const char* bl_name = "Limine";
    const char* bl_version = "";
    if (limine_bootloader_info_response* bi = g_bootloader_request.response) {
        if (bi->name) bl_name = bi->name;
        if (bi->version) bl_version = bi->version;
    }
    log("booted via %s (%s %s)", booted_label, bl_name, bl_version);
    const bool match = (hw.firmware == static_cast<uint32_t>(Firmware::BIOS) && booted[0] == 'b') ||
                       (hw.firmware == static_cast<uint32_t>(Firmware::UEFI) && booted[0] == 'u');
    if (!match) {
        log("warning: this OS declares .firmware(osef::Firmware::%s) but was booted via %s",
            firmware_label(hw.firmware), booted_label);
    }

    // 10. The OS.
    int code = osef_user_main();

    // 11. Done.
    power_off(code);
}

} // namespace
} // namespace osef::bm

// Kernel stack, between a never-mapped guard and its top (linker.ld).
extern "C" char __osef_stack_top[];

extern "C" [[noreturn]] void osef_kmain() {
    // Limine hands over with SSE in an undefined state; turn it on before
    // running anything the compiler may have vectorised.
    osef::bm::cpu::enable_sse();
    // Leave the bootloader's stack (whatever lies below it may be mapped
    // memory) for the kernel stack, whose guard pages are never mapped.
    asm volatile(
        "movq %0, %%rsp\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "callq *%1\n\t"
        "ud2"
        :
        : "r"(__osef_stack_top), "r"(&osef::bm::kmain_body)
        : "memory");
    __builtin_unreachable();
}
