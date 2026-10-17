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
