// DevicesOS: boot test for USB (xHCI enumeration + HID keyboard input) and
// custom interrupts (soft vector + hardware IRQ hook). Used by make test-iso.
// No PS/2 keyboard is declared, so typed input can only arrive over USB.

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("DevicesOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::UEFI)
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PCI | osef::Hw::USB)
    .memory_mb(128));

#include <osef/osef.h>

static uint64_t g_timer_irqs = 0;

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    kernel.interrupts().on_vector(0x80, "test-vector", [](osef::Kernel& k, uint8_t v, uint32_t n) {
        k.print(osef::format("custom vector 0x%02x x%u\n", unsigned(v), n));
    });
    kernel.interrupts().on_irq(0, "timer-hook", [](osef::Kernel&, uint8_t, uint32_t n) { g_timer_irqs += n; });
    kernel.programs().add_fn("ticks", "timer IRQs seen by the hook", [](osef::Context& c) {
        c.println(g_timer_irqs > 0 ? "timer hook: counting" : "timer hook: silent");
        c.exit(0);
    });
    return kernel.run();
}
