#include "model/component_catalog.h"

#include <utility>

namespace osef::editor {

namespace {

constexpr const char* kDecl = "Hardware Declaration";
constexpr const char* kDriver = "Driver";
constexpr const char* kPlanned = "Planned Driver";
constexpr const char* kKernel = "Kernel";
constexpr const char* kUser = "Userland";

// ---- property builders ------------------------------------------------------

Property info(std::string id, std::string label, std::string value, std::string section,
              std::string tooltip = {}) {
    Property p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.s = std::move(value);
    p.section = std::move(section);
    p.tooltip = std::move(tooltip);
    p.type = PropertyType::Info;
    p.read_only = true;
    return p;
}

Property bound_int(std::string id, std::string label, int64_t value, int64_t min, int64_t max, int64_t step,
                   std::string unit, std::string tooltip) {
    Property p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.tooltip = std::move(tooltip);
    p.section = kDecl;
    p.type = PropertyType::Int;
    p.bound = true;
    p.i = value;
    p.min = min;
    p.max = max;
    p.step = step;
    p.unit = std::move(unit);
    return p;
}

Property bound_enum(std::string id, std::string label, int64_t value, std::vector<EnumOption> options,
                    bool radio, std::string tooltip) {
    Property p;
    p.id = std::move(id);
    p.label = std::move(label);
    p.tooltip = std::move(tooltip);
    p.section = kDecl;
    p.type = PropertyType::Enum;
    p.bound = true;
    p.radio = radio;
    p.i = value;
    p.options = std::move(options);
    return p;
}

SourceRef engine(std::string path, std::string label) {
    return SourceRef{SourceRef::Base::Engine, std::move(path), std::move(label), 0};
}

SourceRef project(std::string path, std::string label) {
    return SourceRef{SourceRef::Base::Project, std::move(path), std::move(label), 0};
}

std::unique_ptr<Component> make(std::string id, std::string name, ComponentKind kind, ComponentStatus status,
                                std::string summary, std::string description) {
    auto c = std::make_unique<Component>();
    c->id = std::move(id);
    c->name = std::move(name);
    c->kind = kind;
    c->status = status;
    c->summary = std::move(summary);
    c->description = std::move(description);
    return c;
}

std::unique_ptr<Component> group(std::string id, std::string name, std::string summary, std::string description) {
    return make(std::move(id), std::move(name), ComponentKind::Group, ComponentStatus::Implemented,
                std::move(summary), std::move(description));
}

void make_device_toggle(Component& c, const char* flag) {
    c.toggleable = true;
    c.enabled = true;
    c.toggle_tooltip = std::string("Enabled when osef::Hw::") + flag +
                       " is in .devices(...) of OSEF_HARDWARE. The kernel initializes only declared devices.";
}

// ---- the OS root ------------------------------------------------------------------

std::unique_ptr<Component> os_root(const std::string& os_name) {
    auto c = make("os", os_name, ComponentKind::Os, ComponentStatus::Implemented,
                  "Operating system built with OSEF",
                  "This is your operating system. OSEF is an operating system engine: the kernel, file systems, "
                  "IPC, scheduler and shell are reusable parts, and main.cpp composes them into an OS by adding "
                  "modules, programs, syscalls and devices to an osef::Kernel.\n\n"
                  "The machine the OS runs on is declared once, at the top of main.cpp, with "
                  "OSEF_HARDWARE(osef::Hardware()...). The declaration is checked at compile time "
                  "(static_asserts explain missing settings), compiled into the kernel image (ELF section "
                  ".osef_hardware), read by osef-image to build a BIOS or UEFI ISO, and used at boot: the kernel "
                  "initializes exactly the devices declared there. Properties marked as bound edit that "
                  "declaration directly.\n\n"
                  "The same sources build as a native app (make run) for quick iteration, and as a bare-metal "
                  "x86_64 kernel booted by Limine (make iso, make run-iso).");
    Property name;
    name.id = "name";
    name.label = "Name";
    name.tooltip = ".name(\"...\") in OSEF_HARDWARE: the OS name shown in the boot banner and used for the kernel "
                   "image (1 to 31 characters).";
    name.section = kDecl;
    name.type = PropertyType::String;
    name.bound = true;
    name.s = os_name;
    c->properties.push_back(std::move(name));
    c->properties.push_back(info("project_dir", "Project directory", "", "Project"));
    c->properties.push_back(info("main_file", "Main file", "main.cpp", "Project"));
    c->properties.push_back(info("declaration_status", "Declaration status", "", "Project",
                                 "Where OSEF_HARDWARE is declared, and anything the compiler will reject."));
    c->sources.push_back(project("main.cpp", "OS entry point and hardware declaration"));
    return c;
}

// ---- boot -------------------------------------------------------------------------

std::unique_ptr<Component> boot_group() {
    auto g = group("boot", "Boot", "How the machine starts your OS",
                   "Everything between power-on and your main(): the firmware the ISO is built for, the "
                   "bootloader and the kernel's own boot sequence.");

    auto c = make("boot.firmware", "Firmware", ComponentKind::Firmware, ComponentStatus::Implemented,
                  "Limine boots the kernel via BIOS or UEFI",
                  "The ISO's boot path is exactly the declared firmware: osef-image builds a BIOS-only ISO "
                  "(El Torito no-emulation boot of limine-bios-cd.bin, then `limine bios-install`) or a UEFI-only "
                  "ISO (an EFI system partition image with BOOTX64.EFI). The kernel ELF and limine.conf go to "
                  "/boot; limine.conf gets the boot menu timeout, the serial setting and the resolution from the "
                  "declaration.\n\n"
                  "Limine 11.4.1 loads the higher-half kernel (linked at 0xffffffff80000000, entry osef_kmain) "
                  "with the Limine protocol, base revision 3. The kernel requests the memory map, the HHDM, a "
                  "framebuffer, the firmware type, a 256 KiB stack and the bootloader info.\n\n"
                  "Boot sequence (boot.cpp): enable SSE, check the base revision, read the hardware declaration "
                  "and bring up COM1 for early logging, load the GDT/TSS and IDT, build the heap from the usable "
                  "memory regions (panicking if there is less RAM than .memory_mb()), run global constructors, "
                  "start the framebuffer console, then the PIC, the PIT at 1000 Hz, the keyboard and COM1 "
                  "interrupts. It logs which firmware it was booted with (warning on a mismatch with the "
                  "declaration), calls your main() and powers off with its return value.\n\n"
                  "QEMU runs UEFI images with the bundled OVMF (edk2) firmware and BIOS images with SeaBIOS.");
    c->properties.push_back(bound_enum(
        "firmware", "Firmware", 2,
        {{"BIOS", 1, "Legacy PC BIOS: El Torito no-emulation CD boot"},
         {"UEFI", 2, "UEFI x86_64: boots BOOTX64.EFI from an EFI system partition"}},
        true, ".firmware(osef::Firmware::...) in OSEF_HARDWARE (required)."));
    c->properties.push_back(bound_enum("arch", "Architecture", 1, {{"x86_64", 1, "64-bit x86 (long mode)"}}, false,
                                       ".arch(osef::Arch::...) in OSEF_HARDWARE (required). x86_64 is the only "
                                       "architecture OSEF supports today."));
    c->properties.push_back(bound_int("boot_timeout", "Boot menu timeout", 0, 0, 30, 1, "s",
                                      ".boot_timeout(seconds): how long Limine shows its menu. 0 boots "
                                      "immediately."));
    c->properties.push_back(info("bootloader", "Bootloader", "Limine 11.4.1, Limine protocol, base revision 3",
                                 "Bootloader"));
    c->properties.push_back(info("image", "Image",
                                 "BIOS: El Torito no-emulation ISO / UEFI: EFI system partition ISO",
                                 "Bootloader", "The bootable ISO osef-image builds for the declared firmware."));
    c->sources.push_back(engine("engine/src/baremetal/boot.cpp", "Kernel entry and boot sequence"));
    c->sources.push_back(engine("tools/osef-image/main.cpp", "ISO builder and QEMU runner (osef-image)"));
    c->sources.push_back(engine("engine/src/baremetal/linker.ld", "Kernel linker script"));
    c->sources.push_back(engine("engine/include/osef/hardware.h", "OSEF_HARDWARE and osef::Hardware"));
    g->add_child(std::move(c));
    return g;
}

// ---- hardware ---------------------------------------------------------------------

std::unique_ptr<Component> cpu() {
    auto c = make("hw.cpu", "CPU", ComponentKind::Cpu, ComponentStatus::Implemented,
                  "x86_64, one core, SSE enabled at boot",
                  "The kernel runs in 64-bit long mode on the bootstrap processor only. osef_kmain enables SSE "
                  "first (CR0.EM cleared, CR0.MP set, CR4.OSFXSR and OSXMMEXCPT set), because compiler-generated "
                  "code may use it; the interrupt stubs save and restore the FPU/SSE state with fxsave/fxrstor.\n\n"
                  "gdt.cpp loads a GDT with a null descriptor, kernel code (0x08), kernel data (0x10) and a TSS "
                  "(0x18) whose IST1 stack (16 KiB) handles double faults, so even a kernel stack overflow reaches "
                  "the panic screen. idt.cpp fills all 256 IDT vectors: 0-31 CPU exceptions and 32-47 legacy IRQs "
                  "go through the isr.S stubs to one dispatcher, and 48-255 return immediately.\n\n"
                  "RDRAND seeds /dev/random when CPUID reports it (with a TSC-stirred splitmix64 fallback). "
                  "Multi-core support (SMP: Limine's MP request, per-CPU GDT/TSS, a local APIC timer) is "
                  "planned.");
    c->properties.push_back(info("arch", "Architecture", "x86_64 (long mode, higher-half kernel)", kDriver));
    c->properties.push_back(info("features", "Features", "SSE enabled at boot; RDRAND when available", kDriver));
    c->properties.push_back(info("cores", "Cores", "1 (SMP planned)", kDriver));
    c->properties.push_back(info("tables", "Descriptor tables", "GDT + TSS, IDT 256 vectors", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/gdt.cpp", "GDT and TSS"));
    c->sources.push_back(engine("engine/src/baremetal/idt.cpp", "IDT and interrupt dispatch"));
    c->sources.push_back(engine("engine/src/baremetal/isr.S", "Interrupt entry stubs"));
    c->sources.push_back(engine("engine/src/baremetal/cpu.h", "CPU primitives (port I/O, CR registers, CPUID)"));
    return c;
}

std::unique_ptr<Component> memory() {
    auto c = make("hw.memory", "Memory", ComponentKind::Memory, ComponentStatus::Implemented,
                  "Kernel heap over every usable RAM region",
                  "The declared .memory_mb() is the minimum RAM the OS needs; it is also the VM size used when "
                  "running in QEMU. At boot the kernel adds up the Limine memory map and panics with an "
                  "explanation if the machine has less (allowing max(8 MiB, 1/8) for firmware reservations).\n\n"
                  "Every usable region of at least 64 KiB becomes part of the kernel heap, reached through "
                  "Limine's higher-half direct map (HHDM). heap.cpp keeps an address-ordered free list: first "
                  "fit, block splitting, coalescing with both neighbours on free, 16-byte alignment (larger "
                  "powers of two on request). Every block has a tagged header, so double frees and frees of "
                  "foreign pointers panic instead of corrupting memory. runtime.cpp implements operator "
                  "new/delete on top of it.\n\n"
                  "There is no virtual memory management yet: the kernel runs on Limine's page tables, and in "
                  "base revision 3 the HHDM only maps usable, bootloader-reclaimable, kernel and framebuffer "
                  "memory. Drivers for MMIO devices (PCI BARs, ECAM, ACPI tables) need a page-mapping function "
                  "first.");
    c->properties.push_back(bound_int("memory_mb", "Memory", 128, 32, 65536, 32, "MiB",
                                      ".memory_mb(MiB) in OSEF_HARDWARE: minimum RAM (at least 32), also the QEMU "
                                      "VM size."));
    c->properties.push_back(info("allocator", "Allocator", "free-list, first-fit, coalescing", kDriver));
    c->properties.push_back(info("mapping", "Mapping", "Limine HHDM", kDriver,
                                 "Physical memory is accessed at physical address + the HHDM offset."));
    c->sources.push_back(engine("engine/src/baremetal/heap.cpp", "Kernel heap allocator"));
    c->sources.push_back(engine("engine/src/baremetal/runtime.cpp", "operator new/delete, constructors"));
    c->sources.push_back(engine("engine/src/baremetal/boot.cpp", "Heap setup from the memory map"));
    return c;
}

std::unique_ptr<Component> interrupts() {
    auto c = make("hw.interrupts", "Interrupts", ComponentKind::Interrupts, ComponentStatus::Implemented,
                  "8259 PIC, CPU exceptions isolated per process",
                  "pic.cpp remaps the two cascaded 8259A PICs to vectors 32-47 with every line masked except the "
                  "cascade; drivers unmask their IRQ (timer 0, keyboard 1, COM1 4). Spurious IRQ 7 and 15 are "
                  "detected through the in-service register and get the correct (or no) end-of-interrupt.\n\n"
                  "CPU exceptions are isolated: each process step runs inside a protected region (protect.cpp, "
                  "with the kernel's own setjmp/longjmp in setjmp.S). A page fault, general protection fault, "
                  "invalid opcode or divide error inside a process becomes a readable message (\"page fault at "
                  "0x... rip=0x...\"), the kernel kills that process and keeps running. Exceptions outside a "
                  "process, or inside an IRQ handler, show the panic screen with all registers.\n\n"
                  "Custom interrupts: kernel.interrupts().on_irq(n, ...) hooks IRQ line n (vector 32+n, next to the "
                  "engine's own drivers) and on_vector(v, ...) claims OS vector 0x30-0xFF. Interrupts are counted "
                  "in interrupt context and the handlers run deferred in kernel context, fault-isolated. "
                  "kernel.interrupts().raise(v) (or the `int` command) raises a software interrupt; `irq` lists "
                  "handlers and counts. Add handlers without code in OS Settings > Interrupts.\n\n"
                  "The local APIC and I/O APIC (needed for SMP and for routing PCI interrupts) are planned.");
    c->properties.push_back(info("controller", "Controller", "8259 PIC remapped to vectors 32-47", kDriver));
    c->properties.push_back(info("exceptions", "Exceptions", "isolated per process", kDriver,
                                 "A faulting process is killed with a message; the kernel keeps running."));
    c->sources.push_back(engine("engine/src/baremetal/pic.cpp", "8259A PIC"));
    c->sources.push_back(engine("engine/src/baremetal/idt.cpp", "IDT and IRQ dispatch"));
    c->sources.push_back(engine("engine/src/baremetal/protect.cpp", "Exception routing and fault isolation"));
    c->sources.push_back(engine("engine/src/baremetal/panic.cpp", "Panic screen"));
    return c;
}

std::unique_ptr<Component> timer() {
    auto c = make("hw.timer", "Time", ComponentKind::Timer, ComponentStatus::Implemented,
                  "PIT channel 0 at 1000 Hz drives uptime and sleep",
                  "pit.cpp programs the 8253/8254 PIT channel 0 as a rate generator at 1000 Hz (divisor 1193). "
                  "IRQ 0 adds the exact tick length (999 847 ns) to a nanosecond counter, so uptime does not "
                  "drift. Platform::now_ms() reads it, and sleep_ms() / idle waits use sti; hlt until the next "
                  "interrupt.\n\n"
                  "The kernel's own main loop runs at KernelConfig::tick_hz (100 by default) on top of this "
                  "clock, giving each tick up to quanta_per_tick process steps. A local APIC or HPET timer "
                  "(needed once there is SMP) is planned.");
    c->properties.push_back(info("source", "Source", "PIT channel 0", kDriver));
    c->properties.push_back(info("frequency", "Frequency", "1000 Hz", kDriver));
    c->properties.push_back(info("irq", "IRQ", "0", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/pit.cpp", "PIT driver"));
    return c;
}

std::unique_ptr<Component> rtc() {
    auto c = make("hw.rtc", "Real-Time Clock", ComponentKind::Clock, ComponentStatus::Implemented,
                  "Wall-clock date and time from the CMOS RTC",
                  "The bare-metal driver reads the MC146818-compatible CMOS RTC over ports 0x70/0x71. It waits "
                  "for updates to finish, takes matching samples, handles BCD and 12/24-hour mode, and exposes "
                  "civil time through Platform::read_wall_clock(). The shell's `time` command displays it. "
                  "Hosted builds use the host's local wall clock. The RTC is polled on request; no periodic IRQ "
                  "is enabled.");
    make_device_toggle(*c, "RTC");
    c->properties.push_back(info("source", "Source", "CMOS RTC, polled", kDriver));
    c->properties.push_back(info("command", "Try it", "time", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/rtc.cpp", "CMOS RTC driver"));
    return c;
}

std::unique_ptr<Component> serial() {
    auto c = make("hw.serial", "Serial Port", ComponentKind::Serial, ComponentStatus::Implemented,
                  "16550 UART on COM1: console output and input",
                  "serial.cpp drives a 16550 UART on COM1 (I/O port 0x3F8) as an 8N1 console with the declared "
                  "baud rate (divisor 115200 / baud). A loopback self-test at boot detects whether a UART is "
                  "present. Transmit is polled with a bounded wait, so a missing UART cannot hang the kernel; "
                  "receive is interrupt-driven (IRQ 4) into a 1 KiB ring buffer, also polled as a fallback. "
                  "CR LF from a terminal counts as one Enter and DEL becomes Backspace.\n\n"
                  "Everything the kernel prints goes to the serial port (ANSI escapes pass through), and its "
                  "input is merged with the keyboard's, so `make run-iso` in a terminal (QEMU -serial stdio) "
                  "gives a full console. Panics are written here first, because it needs no other "
                  "subsystem.\n\n"
                  "Serial or Framebuffer is required: an OS needs at least one console device.");
    make_device_toggle(*c, "Serial");
    c->properties.push_back(bound_enum("baud", "Baud rate", 115200,
                                       {{"115200", 115200, ""},
                                        {"57600", 57600, ""},
                                        {"38400", 38400, ""},
                                        {"19200", 19200, ""},
                                        {"9600", 9600, ""}},
                                       false,
                                       ".serial_baud(rate) in OSEF_HARDWARE: must divide 115200. Omitted from the "
                                       "declaration when it is 115200 (the default)."));
    c->properties.push_back(info("port", "Port", "COM1 0x3F8", kDriver));
    c->properties.push_back(info("irq", "IRQ", "4", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/serial.cpp", "16550 UART driver"));
    return c;
}

std::unique_ptr<Component> display() {
    auto c = make("hw.display", "Display", ComponentKind::Display, ComponentStatus::Implemented,
                  "Framebuffer text console",
                  "fbconsole.cpp draws a text console on the linear framebuffer Limine sets up: the public-domain "
                  "font8x8 glyphs (scaled 2x on framebuffers at least 1600 pixels wide), a VGA-style 16-colour "
                  "palette built from the framebuffer's channel masks, and scrolling by moving pixel rows. It "
                  "understands the ANSI subset the shell uses: SGR colours 30-37/90-97 (and backgrounds), bold "
                  "and reset, ESC[2J, ESC[K, ESC[H and cursor movement, plus \\r \\n \\b \\t.\n\n"
                  "Only 32 bits-per-pixel RGB framebuffers are supported. .resolution(width, height) asks Limine "
                  "for a mode (limine.conf `resolution:`); 0 x 0 keeps the firmware's default. The console needs "
                  "no heap, so the panic screen can use it at any time.\n\n"
                  "Serial or Framebuffer is required: an OS needs at least one console device. Graphics beyond "
                  "text (a pixel API, double buffering, a GPU driver such as virtio-gpu) are planned.");
    make_device_toggle(*c, "Framebuffer");
    Property res;
    res.id = "resolution";
    res.label = "Resolution";
    res.tooltip = ".resolution(width, height) in OSEF_HARDWARE: preferred framebuffer mode. 0 x 0 = firmware "
                  "default (omitted from the declaration).";
    res.section = kDecl;
    res.type = PropertyType::Resolution;
    res.bound = true;
    res.min = 0;
    res.max = 8192;
    res.unit = "px";
    c->properties.push_back(std::move(res));
    c->properties.push_back(info("console", "Console", "font8x8 text console, ANSI colors", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/fbconsole.cpp", "Framebuffer console"));
    c->sources.push_back(engine("engine/src/baremetal/font8x8.h", "8x8 bitmap font (public domain)"));
    return c;
}

std::unique_ptr<Component> keyboard() {
    auto c = make("hw.keyboard", "Keyboard", ComponentKind::Keyboard, ComponentStatus::Implemented,
                  "PS/2 keyboard via the i8042 controller",
                  "keyboard.cpp configures the i8042 PS/2 controller (data port 0x60, status/command port 0x64): "
                  "it disables both ports while reprogramming, enables port 1 with its IRQ and scancode set 1 "
                  "translation, and keeps the mouse port and IRQ 12 off. IRQ 1 decodes make/break codes into ASCII "
                  "in a 256-byte ring buffer, with Shift, Caps Lock, Ctrl+letter, Backspace, Tab, Enter and the "
                  "keypad Enter and /. Arrow and function keys are not mapped yet.\n\n"
                  "Keyboard input is merged with serial input; the platform layer does line editing and echoes "
                  "to every console. UEFI machines usually emulate a PS/2 keyboard for USB keyboards; a native "
                  "USB HID driver needs the planned USB stack.");
    make_device_toggle(*c, "PS2Keyboard");
    c->properties.push_back(info("interface", "Interface", "PS/2, scancode set 1", kDriver));
    c->properties.push_back(info("irq", "IRQ", "1", kDriver));
    c->properties.push_back(info("ports", "Ports", "0x60 / 0x64", kDriver));
    Property layout;
    layout.id = "layout";
    layout.label = "Layout";
    layout.tooltip = "Only the US layout is built in.";
    layout.section = kDriver;
    layout.type = PropertyType::Enum;
    layout.read_only = true;
    layout.i = 0;
    layout.options = {{"US", 0, "US QWERTY"}};
    c->properties.push_back(std::move(layout));
    c->sources.push_back(engine("engine/src/baremetal/keyboard.cpp", "PS/2 keyboard driver"));
    return c;
}

std::unique_ptr<Component> mouse() {
    auto c = make("hw.mouse", "Mouse", ComponentKind::Mouse, ComponentStatus::Planned,
                  "PS/2 mouse on the i8042 aux port",
                  "Not in the engine yet. A PS/2 mouse driver uses the i8042's second (aux) port: enable it "
                  "(command 0xA8), set bit 1 (IRQ 12 on) and clear bit 5 (aux clock off) of the controller "
                  "configuration byte (commands 0x20/0x60), then send device commands prefixed with 0xD4: 0xF6 "
                  "set defaults, 0xF4 enable data reporting (each answered with 0xFA). Packets are 3 bytes "
                  "(buttons, X/Y sign and overflow bits, dx, dy); the IntelliMouse wheel is enabled by setting "
                  "the sample rate to 200, 100, 80 and checking that the device id becomes 3 (4-byte packets).\n\n"
                  "Today keyboard.cpp deliberately disables the aux port and drops aux bytes (status bit 5), and "
                  "the IDT dispatcher (idt.cpp) only routes IRQs 0, 1 and 4. A mouse driver would take over the "
                  "controller configuration, unmask IRQ 12 on the slave PIC, add it to the dispatcher, and "
                  "expose events as a device (e.g. /dev/mouse). It would also need a new osef::Hw flag in "
                  "hardware.h. USB mice (HID boot protocol) come with the planned USB stack.");
    c->properties.push_back(info("interface", "Interface", "PS/2 aux port, IRQ 12; USB HID later", kPlanned));
    return c;
}

std::unique_ptr<Component> usb() {
    auto c = make("hw.usb", "USB", ComponentKind::Usb, ComponentStatus::Partial,
                  "xHCI host controller, device enumeration, HID boot keyboards",
                  "usb.cpp drives an xHCI controller (PCI class 0x0C/0x03, prog-if 0x30): it enables memory decoding "
                  "and bus mastering, maps BAR0 through the kernel's MMIO window (vmm.cpp; MMIO is not in Limine's "
                  "HHDM), takes the controller from the firmware via the USB legacy support capability, resets it, "
                  "and sets up the device context base address array, scratchpad buffers, a command ring and one "
                  "event ring (polled; interrupter 0 has IE cleared).\n\n"
                  "Each connected root-hub port is reset, gets a slot (Enable Slot), an address (Address Device, "
                  "EP0 max packet fixed up with Evaluate Context) and is described from its device, configuration "
                  "and string descriptors: `lsusb` lists vendor/product ids, names, class and speed.\n\n"
                  "HID boot-protocol keyboards are configured (SET_CONFIGURATION, SET_PROTOCOL boot, Configure "
                  "Endpoint for the interrupt IN endpoint); their 8-byte reports become console input next to "
                  "PS/2 and serial, so a machine with only a USB keyboard works.\n\n"
                  "Not yet: external hubs, mice, mass storage (bulk-only transport), MSI-X interrupts and hot plug "
                  "after boot. In QEMU, `make run-iso` adds -device qemu-xhci with usb-kbd and usb-tablet.");
    make_device_toggle(*c, "USB");
    c->properties.push_back(info("host_controller", "Host controller", "xHCI 1.x (polled event ring)", kDriver));
    c->properties.push_back(info("classes", "Class drivers", "HID boot keyboard (console input)", kDriver));
    c->properties.push_back(info("command", "Try it", "lsusb", kDriver));
    c->properties.push_back(info("planned", "Planned", "hubs, HID mouse, mass storage, MSI-X, hot plug", kPlanned));
    c->sources.push_back(engine("engine/src/baremetal/usb.cpp", "xHCI driver"));
    c->sources.push_back(engine("engine/src/baremetal/vmm.cpp", "MMIO mapping"));
    return c;
}

std::unique_ptr<Component> storage() {
    auto c = make("hw.storage", "Storage", ComponentKind::Storage, ComponentStatus::Planned,
                  "Disk controllers: AHCI, NVMe, ATA",
                  "Not in the engine yet: on bare metal the file system lives in RAM (ramfs) and is lost at "
                  "power-off; hosted builds can mount a host folder with HostFS. A block device driver would "
                  "plug into the VFS as a FileSystem (see engine/include/osef/vfs.h), with a disk format such as "
                  "FAT32 or ext2 on top.\n\n"
                  "Candidates: ATA PIO on the legacy ports 0x1F0-0x1F7 (simplest, no PCI needed); AHCI for SATA "
                  "(PCI class 0x01/0x06, MMIO registers at BAR5, command lists and FIS receive areas per port; "
                  "Serial ATA AHCI 1.3.1 specification); NVMe (PCI class 0x01/0x08, admin and I/O submission/"
                  "completion queues in MMIO at BAR0; NVM Express Base Specification); and virtio-blk for VMs "
                  "(OASIS VIRTIO 1.2). All but ATA PIO need the PCI component.");
    c->properties.push_back(info("controllers", "Controllers", "AHCI SATA, NVMe, ATA PIO", kPlanned));
    return c;
}

std::unique_ptr<Component> pci() {
    auto c = make("hw.pci", "PCI", ComponentKind::Pci, ComponentStatus::Implemented,
                  "PCI / PCIe bus enumeration",
                  "The x86_64 bare-metal driver enumerates functions through PCI configuration mechanism #1 "
                  "(ports 0xCF8/0xCFC), including multifunction devices, and reports B:D.F, vendor/device ids "
                  "and class codes through Platform::pci_devices(). `lspci` lists discovered functions. This "
                  "baseline supports the legacy PCI configuration window only: it does not configure BARs, "
                  "enable bus mastering, or drive PCI devices.");
    make_device_toggle(*c, "PCI");
    c->properties.push_back(info("access", "Access", "PCI mechanism #1, 0xCF8 / 0xCFC", kDriver));
    c->properties.push_back(info("command", "Try it", "lspci", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/pci.cpp", "PCI enumeration"));
    return c;
}

std::unique_ptr<Component> network() {
    auto c = make("hw.network", "Network", ComponentKind::Network, ComponentStatus::Planned,
                  "Network adapters and a TCP/IP stack",
                  "Not in the engine yet (needs PCI). virtio-net is the easiest adapter in QEMU (OASIS VIRTIO "
                  "1.2: PCI vendor 0x1AF4, feature negotiation, receive and transmit virtqueues); the Intel "
                  "e1000 (82540EM, QEMU's -device e1000) is the classic real-hardware choice: MMIO registers at "
                  "BAR0, receive/transmit descriptor rings, MAC address from the EEPROM/RAL registers.\n\n"
                  "Above the driver: Ethernet, ARP, IPv4, ICMP, UDP (DHCP, DNS) and eventually TCP. OSEF's IPC "
                  "services fit well here: run the stack as a \"net\" service process that programs talk to "
                  "through messages.");
    c->properties.push_back(info("adapters", "Adapters", "virtio-net, Intel e1000", kPlanned));
    return c;
}

std::unique_ptr<Component> audio() {
    auto c = make("hw.audio", "Audio", ComponentKind::Audio, ComponentStatus::Planned,
                  "Sound output: Intel HDA, AC'97",
                  "Not in the engine yet (needs PCI). Intel High Definition Audio (PCI class 0x04/0x03) is what "
                  "most PCs and QEMU's -device intel-hda provide: reset the controller via MMIO at BAR0, discover "
                  "codecs through CORB/RIRB command rings, walk the codec widget graph to an output pin, and "
                  "stream PCM from a buffer descriptor list (Intel HDA specification rev. 1.0a). AC'97 (QEMU "
                  "-device AC97) is simpler: port I/O mixer and bus master registers with a 32-entry buffer "
                  "descriptor list.\n\n"
                  "Expose playback as a device (e.g. /dev/audio accepting PCM samples) and a mixer through "
                  "syscalls.");
    c->properties.push_back(info("devices", "Devices", "Intel HDA, AC'97", kPlanned));
    return c;
}

std::unique_ptr<Component> power() {
    auto c = make("hw.power", "Power", ComponentKind::Power, ComponentStatus::Implemented,
                  "ACPI S5 soft-off, with emulator and test fallbacks",
                  "When main() returns (or `shutdown` runs in the shell), the kernel prints \"System halted\", "
                  "tries a validated ACPI S5 soft-off method discovered from the Limine RSDP, XSDT/RSDT, FADT "
                  "and the DSDT's _S5 package. It enables ACPI mode through SMI_CMD when needed and writes the "
                  "PM1 control registers. QEMU's isa-debug-exit is retained for automated test exit codes; "
                  "known emulator ports remain as fallback if ACPI is unavailable. If none works, the CPU halts.");
    c->properties.push_back(info("shutdown", "Shutdown", "ACPI S5; emulator fallback", kDriver));
    c->properties.push_back(info("acpi", "ACPI", "RSDP, FADT, DSDT _S5 soft-off", kDriver));
    c->sources.push_back(engine("engine/src/baremetal/panic.cpp", "Panic screen and power off"));
    c->sources.push_back(engine("engine/src/baremetal/acpi.cpp", "ACPI soft-off"));
    return c;
}

std::unique_ptr<Component> hardware_group() {
    auto g = group("hw", "Hardware", "Devices and CPU features of the machine",
                   "The machine your OS drives. Implemented components work in the bare-metal kernel today; "
                   "the working Serial Port, Display, Keyboard, Real-Time Clock and PCI bus can be switched on "
                   "and off in the OSEF_HARDWARE declaration. Planned components are places to expand the engine: "
                   "select one and use \"Create Driver Stub\" to start a driver module in your project.");
    g->add_child(cpu());
    g->add_child(memory());
    g->add_child(interrupts());
    g->add_child(timer());
    g->add_child(rtc());
    g->add_child(serial());
    g->add_child(display());
    g->add_child(keyboard());
    g->add_child(mouse());
    g->add_child(usb());
    g->add_child(storage());
    g->add_child(pci());
    g->add_child(network());
    g->add_child(audio());
    g->add_child(power());
    return g;
}

// ---- kernel -----------------------------------------------------------------------

std::unique_ptr<Component> kernel_group() {
    auto g = group("kernel", "Kernel", "The OSEF engine runtime",
                   "osef::Kernel owns every subsystem and runs the main loop, like a game engine. These parts are "
                   "portable: the same code runs hosted (as a native app) and on bare metal.");

    auto sched = make("kernel.scheduler", "Scheduler", ComponentKind::Scheduler, ComponentStatus::Implemented,
                      "Pluggable policy that picks the next process",
                      "Each tick the kernel asks the scheduler which ready process gets the next quantum "
                      "(Scheduler::pick). Three policies ship with the engine: RoundRobinScheduler (the default: "
                      "whoever waited longest), PriorityScheduler (highest priority wins, ties round-robin; low "
                      "priorities can starve) and LotteryScheduler (priority + 1 tickets each, a random ticket "
                      "wins).\n\n"
                      "Choose one with kernel.set_scheduler(make_unique<PriorityScheduler>()), or write your own "
                      "by subclassing osef::Scheduler. The shell's `sched` command shows the active one.");
    sched->properties.push_back(info("policies", "Policies", "round-robin (default), priority, lottery", kKernel));
    sched->properties.push_back(info("configured_in", "Configured in", "kernel.set_scheduler(...)", kKernel));
    sched->sources.push_back(engine("engine/src/scheduler.cpp", "Scheduler policies"));
    sched->sources.push_back(engine("engine/include/osef/scheduler.h", "Scheduler interface"));
    g->add_child(std::move(sched));

    auto procs = make("kernel.processes", "Processes", ComponentKind::Processes, ComponentStatus::Implemented,
                      "Cooperative, step-based processes",
                      "Processes are the \"game objects\" of an OSEF OS. The kernel calls Process::step() once per "
                      "quantum; a step does a little work and returns. Blocking calls (read_line, receive, sleep, "
                      "wait) mark the process as waiting and return, and step() runs again once the condition is "
                      "met. No threads and no context switches, so the engine stays portable.\n\n"
                      "Each process has a pid, a parent, arguments, a working directory, a priority and a state "
                      "(Ready, Sleeping, WaitInput, WaitMessage, WaitChild, Zombie). A process that faults or calls "
                      "Context::abort() is killed with a message and the rest of the system keeps running.");
    procs->properties.push_back(info("model", "Model", "cooperative, step-based", kKernel));
    procs->properties.push_back(info("fault_isolation", "Fault isolation", "yes", kKernel,
                                     "Hosted: exceptions; bare metal: CPU exceptions are caught per step."));
    procs->sources.push_back(engine("engine/src/kernel.cpp", "Kernel: processes and main loop"));
    procs->sources.push_back(engine("engine/include/osef/process.h", "Process and ProcessInfo"));
    procs->sources.push_back(engine("engine/include/osef/kernel.h", "Kernel and Context API"));
    g->add_child(std::move(procs));

    auto ipc = make("kernel.ipc", "IPC & Services", ComponentKind::Ipc, ComponentStatus::Implemented,
                    "Mailboxes and a service name registry",
                    "Every process has a mailbox of Messages (sender pid, type, text data). Context::send() posts "
                    "to a pid and receive() takes the next message, sleeping while the mailbox is empty.\n\n"
                    "Services are named processes in microkernel style: a daemon calls "
                    "register_service(\"time\"), and clients find it with lookup(\"time\"). Names are released when "
                    "the process dies. The shell's `services` and `send` commands work with them.");
    ipc->properties.push_back(info("mailboxes", "Mailboxes", "one per process", kKernel));
    ipc->properties.push_back(info("services", "Service registry", "register_service / lookup by name", kKernel));
    ipc->sources.push_back(engine("engine/src/ipc.cpp", "Mailboxes and services"));
    ipc->sources.push_back(engine("engine/include/osef/ipc.h", "IPC interface"));
    g->add_child(std::move(ipc));

    auto sys = make("kernel.syscalls", "System Calls", ComponentKind::Syscalls, ComponentStatus::Implemented,
                    "Named system calls: your OS defines its ABI",
                    "System calls are registered by name with kernel.syscalls().define(name, description, fn) and "
                    "invoked with Context::syscall(name, args). Results carry an errno-style code (negative on "
                    "error, -38 ENOSYS for unknown calls) and text data. Every call goes through one invoke(), so "
                    "tracing (set_tracer) and permission checks fit in one place.\n\n"
                    "Built in: getpid, getppid, uptime, uname, kill and spawn. The shell's `sys` command lists and "
                    "calls them.");
    sys->properties.push_back(info("builtins", "Built-ins", "getpid, getppid, uptime, uname, kill, spawn", kKernel));
    sys->sources.push_back(engine("engine/include/osef/syscall.h", "Syscall table"));
    sys->sources.push_back(engine("engine/src/ipc.cpp", "SyscallTable::invoke"));
    sys->sources.push_back(engine("engine/src/kernel.cpp", "Built-in syscalls"));
    g->add_child(std::move(sys));

    auto vfs = make("kernel.vfs", "File Systems", ComponentKind::FileSystem, ComponentStatus::Implemented,
                    "Virtual file system with mount points",
                    "The VFS resolves paths through mount points to FileSystem implementations. The root / is a "
                    "RamFS (in memory) and /dev is DevFS, the built-in devices. Hosted builds can also mount a "
                    "host folder with HostFS (myos mounts one at /disk), which persists between runs.\n\n"
                    "Implement osef::FileSystem to add your own (a disk format once there is a storage driver, "
                    "or a synthetic file system like /proc). Shell commands: ls, cat, cd, pwd, mkdir, touch, rm, "
                    "mounts.");
    vfs->properties.push_back(info("mounts", "Mounts", "/ ramfs, /dev devfs", kKernel));
    vfs->sources.push_back(engine("engine/src/vfs.cpp", "VFS, RamFS and DevFS"));
    vfs->sources.push_back(engine("engine/include/osef/vfs.h", "File system interfaces"));
    vfs->sources.push_back(engine("engine/include/osef/hostfs.h", "HostFS (hosted only)"));
    g->add_child(std::move(vfs));

    auto dev = make("kernel.devices", "Devices (/dev)", ComponentKind::Devices, ComponentStatus::Implemented,
                    "Character devices under /dev",
                    "A device is a description plus read and write callbacks (osef::Device); leave one empty to "
                    "make it write-only or read-only. Register one with kernel.register_device(name, {...}) and it "
                    "appears as /dev/<name>.\n\n"
                    "Built in: null (discards writes, reads empty), console (writes to the system console), random "
                    "(64 random bits as hex) and uptime (milliseconds since boot). Modules can add their own, like "
                    "myos's \"led\".");
    dev->properties.push_back(info("builtins", "Built-ins", "null, console, random, uptime", kKernel));
    dev->sources.push_back(engine("engine/src/kernel.cpp", "Built-in devices"));
    dev->sources.push_back(engine("engine/include/osef/vfs.h", "osef::Device and DevFS"));
    g->add_child(std::move(dev));
    return g;
}

// ---- userland ---------------------------------------------------------------------

std::unique_ptr<Component> user_group() {
    auto g = group("user", "Userland", "Programs and modules your OS ships",
                   "What runs on top of the kernel: the shell, the programs registered in main.cpp and the "
                   "modules (engine plugins) the OS adds.");

    auto shell = make("user.shell", "Shell", ComponentKind::Shell, ComponentStatus::Implemented,
                      "The OSEF shell (program \"sh\")",
                      "kernel.add_module<ShellModule>() registers the program \"sh\", an interactive shell with "
                      "built-in commands: help, echo, ls, cat, cd, pwd, mkdir, touch, rm, mounts, ps, kill, send, "
                      "services, sys, mods, sched, hw, mem, uptime, uname, clear, exit and shutdown. Any other word "
                      "runs the registered program with that name, with arguments, in the foreground (or in the "
                      "background with a trailing &). Built-in output can be redirected with > and >>.\n\n"
                      "`hw` shows the OSEF_HARDWARE declaration the kernel was built with.");
    shell->properties.push_back(info("builtins", "Builtins", "help, ls, cat, ps, hw, mem, sys, shutdown, ...", kUser));
    shell->sources.push_back(engine("engine/src/shell.cpp", "Shell and its commands"));
    shell->sources.push_back(engine("engine/include/osef/shell.h", "ShellModule"));
    g->add_child(std::move(shell));

    auto programs = make("user.programs", "Programs", ComponentKind::Programs, ComponentStatus::Implemented,
                         "Programs registered in main.cpp",
                         "Programs are the OS's executables: named factories that create a fresh Process when run "
                         "(from the shell, Context::spawn or the init program). Register a class with "
                         "programs.add<T>(\"name\", \"description\") or a lambda with "
                         "programs.add_fn(\"name\", \"description\", [](Context& c) { ...; c.exit(0); }).\n\n"
                         "The children below are discovered from main.cpp as you edit it.");
    programs->properties.push_back(info("count", "Count", "0", kUser));
    programs->sources.push_back(project("main.cpp", "Program registrations"));
    programs->sources.push_back(engine("engine/include/osef/program.h", "Program registry"));
    g->add_child(std::move(programs));

    auto modules = make("user.modules", "Modules", ComponentKind::Modules, ComponentStatus::Implemented,
                        "Engine plugins added with add_module",
                        "Modules are engine plugins, the OS equivalent of a game engine's systems. A module can "
                        "register programs, syscalls, devices and file systems in on_load(), run kernel-level logic "
                        "every tick in on_tick(), and clean up in on_unload(). Add one with "
                        "kernel.add_module<T>(); find it again with kernel.find_module<T>().\n\n"
                        "The children below are discovered from main.cpp as you edit it.");
    modules->properties.push_back(info("count", "Count", "0", kUser));
    modules->sources.push_back(project("main.cpp", "add_module calls"));
    modules->sources.push_back(engine("engine/include/osef/module.h", "osef::Module"));
    g->add_child(std::move(modules));
    return g;
}

} // namespace

std::unique_ptr<Component> build_component_catalog(const std::string& os_name) {
    auto root = os_root(os_name);
    root->add_child(boot_group());
    root->add_child(hardware_group());
    root->add_child(kernel_group());
    root->add_child(user_group());
    return root;
}

} // namespace osef::editor
