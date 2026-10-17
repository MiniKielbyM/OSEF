// Default Platform behavior shared by every target (hosted and bare metal).

#include <osef/platform.h>

namespace osef {

bool Platform::run_protected(void (*fn)(void*), void* arg, String&) {
    fn(arg); // no isolation available: a fault stops the machine
    return true;
}

void Platform::fault(StringView message) {
    write("\nprocess fault outside run_protected: ");
    write(message);
    write("\n");
    assert_fail(__FILE__, __LINE__, "Platform::fault called without fault isolation");
}

} // namespace osef

namespace osef {

// Simulated interrupts (hosted builds, tests): only software-raised vectors.
bool Platform::claim_interrupt(uint8_t vector) {
    if (vector < 32) return false;
    claimed_[vector] = true;
    pending_[vector] = 0;
    return true;
}

void Platform::release_interrupt(uint8_t vector) {
    claimed_[vector] = false;
    pending_[vector] = 0;
}

uint32_t Platform::take_interrupts(uint8_t vector) {
    const uint32_t n = pending_[vector];
    pending_[vector] = 0;
    return n;
}

bool Platform::raise_interrupt(uint8_t vector) {
    if (vector < 48) return false;
    if (claimed_[vector]) ++pending_[vector];
    return true;
}

} // namespace osef
