// The kernel's view of the OSEF_HARDWARE declaration (hosted and bare metal).

#include <osef/hardware.h>

namespace osef {

const HardwareSpec& hardware() { return osef_hardware_declaration; }

const char* to_string(Arch arch) {
    switch (arch) {
        case Arch::x86_64: return "x86_64";
        case Arch::Unspecified: break;
    }
    return "unspecified";
}

const char* to_string(Firmware firmware) {
    switch (firmware) {
        case Firmware::BIOS: return "BIOS";
        case Firmware::UEFI: return "UEFI";
        case Firmware::Unspecified: break;
    }
    return "unspecified";
}

} // namespace osef
