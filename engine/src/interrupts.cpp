#include <osef/interrupts.h>
#include <osef/kernel.h>

namespace osef {

bool Interrupts::add(uint8_t vector, StringView name, InterruptFn fn) {
    if (!fn) return false;
    if (!entries_.contains(vector) && !k_.platform().claim_interrupt(vector)) return false;
    Entry& e = entries_[vector];
    e.name = String(name);
    e.fn = osef::move(fn);
    return true;
}

bool Interrupts::on_irq(uint8_t irq, StringView name, InterruptFn fn) {
    if (irq > 15) return false;
    return add(irq_vector(irq), name, osef::move(fn));
}

bool Interrupts::on_vector(uint8_t vector, StringView name, InterruptFn fn) {
    if (vector < kFirstFreeVector) return false;
    return add(vector, name, osef::move(fn));
}

bool Interrupts::remove(uint8_t vector) {
    if (entries_.erase(vector) == 0) return false;
    k_.platform().release_interrupt(vector);
    return true;
}

bool Interrupts::raise(uint8_t vector) {
    if (vector < kFirstFreeVector) return false;
    return k_.platform().raise_interrupt(vector);
}

uint64_t Interrupts::count(uint8_t vector) const {
    auto it = entries_.find(vector);
    return it == entries_.end() ? 0 : it->second.count;
}

bool Interrupts::hardware() const { return k_.platform().hardware_interrupts(); }

Vector<InterruptInfo> Interrupts::list() const {
    Vector<InterruptInfo> out;
    for (const auto& [vector, e] : entries_) {
        InterruptInfo info;
        info.vector = vector;
        info.name = e.name;
        info.irq = vector >= kFirstIrqVector && vector < kFirstFreeVector;
        info.count = e.count;
        out.push_back(osef::move(info));
    }
    return out;
}

namespace {
struct Delivery {
    Kernel* kernel;
    InterruptFn* fn;
    uint8_t vector;
    uint32_t count;
};
} // namespace

void Interrupts::poll() {
    // Collect first: a handler may add or remove handlers (Map iterators).
    Vector<Pair<uint8_t, uint32_t>> ready;
    for (const auto& [vector, e] : entries_) {
        const uint32_t n = k_.platform().take_interrupts(vector);
        if (n) ready.push_back(Pair<uint8_t, uint32_t>{vector, n});
    }
    for (const auto& [vector, n] : ready) {
        auto it = entries_.find(vector);
        if (it == entries_.end()) continue;
        it->second.count += n;
        InterruptFn fn = it->second.fn;  // copies: the entry may be removed by the handler
        const String name = it->second.name;
        Delivery d{&k_, &fn, vector, n};
        String fault;
        const bool ok = k_.platform().run_protected(
            [](void* arg) {
                auto* d = static_cast<Delivery*>(arg);
                (*d->fn)(*d->kernel, d->vector, d->count);
            },
            &d, fault);
        if (!ok)
            k_.log(LogLevel::Error,
                   format("interrupt 0x%02x (%s) handler crashed: %s", unsigned(vector), name.c_str(), fault.c_str()));
    }
}

} // namespace osef
