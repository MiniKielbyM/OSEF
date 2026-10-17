#pragma once
// Reads and writes the OSEF_HARDWARE(...) declaration at the top of an OS's
// main.cpp (see engine/include/osef/hardware.h), so the Inspector can edit
// hardware settings and the code stays the single source of truth.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

// Plain-data mirror of the osef::Hardware() builder.
struct HardwareDecl {
    std::string name = "OSEF";
    std::string arch;          // "x86_64", or "" when .arch() is missing
    std::string firmware;      // "BIOS", "UEFI", or "" when .firmware() is missing
    bool serial = false;       // osef::Hw::Serial
    bool framebuffer = false;  // osef::Hw::Framebuffer
    bool ps2_keyboard = false; // osef::Hw::PS2Keyboard
    bool rtc = false;          // osef::Hw::RTC
    bool pci = false;          // osef::Hw::PCI
    uint32_t memory_mb = 128;
    uint32_t serial_baud = 115200;
    uint32_t fb_width = 0;     // 0 x 0 = firmware default
    uint32_t fb_height = 0;
    uint32_t boot_timeout_s = 0;

    bool operator==(const HardwareDecl&) const = default;
};

struct HardwareParse {
    bool found = false;        // an OSEF_HARDWARE( ... ); statement exists outside comments/strings
    HardwareDecl decl;         // defaults for anything not declared
    size_t begin = 0;          // byte offset of "OSEF_HARDWARE"
    size_t end = 0;            // byte offset just past the terminating ';'
    int line = 0;              // 1-based line of "OSEF_HARDWARE"
    // Human-readable issues: unknown builder calls, unparsable arguments,
    // and missing required settings (architecture, firmware, console device).
    std::vector<std::string> problems;
};

HardwareParse parse_hardware_decl(std::string_view source);

// Canonical declaration text (no trailing newline), e.g.
//
//   OSEF_HARDWARE(osef::Hardware()
//       .name("MyOS")
//       .arch(osef::Arch::x86_64)
//       .firmware(osef::Firmware::UEFI)
//       .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)
//       .memory_mb(128));
//
// .serial_baud/.resolution/.boot_timeout are emitted only when they differ
// from the defaults. Missing required settings (empty arch/firmware, no
// devices) are omitted, so the compiler's static_assert explains what to add.
std::string generate_hardware_decl(const HardwareDecl& decl);

// Returns `source` with its declaration replaced by generate_hardware_decl(decl)
// or, when there is none, inserted after `#include <osef/hardware.h>` (adding
// that include after the file's leading comment block if it is missing).
// Everything outside the declaration (and the added include) is preserved
// byte for byte.
std::string apply_hardware_decl(std::string_view source, const HardwareDecl& decl);

// True when the declaration in `source` contains builder calls or arguments
// the parser could not read (they are listed in HardwareParse::problems), so
// apply_hardware_decl() would drop them when it rewrites the declaration.
bool hardware_decl_has_unparsed_code(std::string_view source);

} // namespace osef::editor
