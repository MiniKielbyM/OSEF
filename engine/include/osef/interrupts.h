#pragma once
// Custom interrupts: let an OS react to hardware IRQ lines and to its own
// interrupt vectors without touching the low-level interrupt code.
//
//   kernel.interrupts().on_irq(12, "mouse", [](osef::Kernel& k, uint8_t, uint32_t n) { ... });
//   kernel.interrupts().on_vector(0x80, "my-call", [](osef::Kernel& k, uint8_t v, uint32_t n) { ... });
//   kernel.interrupts().raise(0x80);   // software interrupt (`int $0x80` on bare metal)
//
// Vector map: 0-31 CPU exceptions (reserved; faults are handled by the kernel),
// 32-47 legacy IRQ lines 0-15, 48-255 free for the OS.
//
// Handlers are *deferred*: the interrupt itself only counts, and the kernel
// calls the handler from its main loop (next tick) with the number of
// interrupts since the last call. So handlers may use the whole kernel API,
// and a faulting handler is isolated like a process.

#include <osef/core.h>

namespace osef {

class Kernel;

using InterruptFn = Function<void(Kernel& kernel, uint8_t vector, uint32_t count)>;

struct InterruptInfo {
    uint8_t vector = 0;
    String name;
    bool irq = false;        // a hardware IRQ line (vector 32-47)
    uint64_t count = 0;      // interrupts delivered so far
};

class Interrupts {
public:
    static constexpr uint8_t kFirstIrqVector = 32;
    static constexpr uint8_t kFirstFreeVector = 48;
    static constexpr uint8_t irq_vector(uint8_t irq) { return static_cast<uint8_t>(kFirstIrqVector + irq); }

    explicit Interrupts(Kernel& kernel) : k_(kernel) {}

    // Hardware IRQ line 0-15 (the engine's own IRQ 0/1/4 drivers keep running).
    bool on_irq(uint8_t irq, StringView name, InterruptFn fn);
    // OS vector 48-255.
    bool on_vector(uint8_t vector, StringView name, InterruptFn fn);
    bool remove(uint8_t vector);

    // Software interrupt on an OS vector (48-255). Returns false for others.
    bool raise(uint8_t vector);

    uint64_t count(uint8_t vector) const;
    Vector<InterruptInfo> list() const;
    bool hardware() const;   // true on bare metal (real IRQ lines)

    // Called by the kernel every tick: delivers pending interrupts.
    void poll();

private:
    struct Entry {
        String name;
        InterruptFn fn;
        uint64_t count = 0;
    };
    bool add(uint8_t vector, StringView name, InterruptFn fn);
    Kernel& k_;
    Map<uint8_t, Entry> entries_;
};

} // namespace osef
