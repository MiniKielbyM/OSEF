#pragma once
// OSEF hardware declaration.
//
// Every OSEF operating system must declare the machine it runs on at the top
// of its main .cpp file, before any other OSEF code:
//
//   #include <osef/hardware.h>
//
//   OSEF_HARDWARE(osef::Hardware()
//       .name("MyOS")
//       .arch(osef::Arch::x86_64)                  // required
//       .firmware(osef::Firmware::UEFI)            // required: BIOS or UEFI
//       .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)
//       .memory_mb(128));
//
//   #include <osef/osef.h>
//
// The declaration is:
//   * checked at compile time: missing or invalid fields are compile errors;
//   * required at link time: an OS without one fails to link with
//     "undefined symbol: osef_hardware_declaration";
//   * compiled into the kernel image (ELF section .osef_hardware), where the
//     image tool (osef-image) reads it to build a BIOS or UEFI bootable ISO;
//   * used at boot: the kernel initializes exactly the devices declared here.
//
// This header is freestanding (no standard library) and is shared with the
// host-side tools, so HardwareSpec's layout is part of the image format.

#include <stdint.h>

namespace osef {

enum class Arch : uint32_t {
    Unspecified = 0,
    x86_64 = 1,
};

enum class Firmware : uint32_t {
    Unspecified = 0,
    BIOS = 1,  // legacy PC BIOS (El Torito no-emulation boot)
    UEFI = 2,  // UEFI x86_64 (EFI system partition boot)
};

// Device flags for Hardware::devices().
namespace Hw {
enum : uint32_t {
    Serial = 1u << 0,       // 16550 UART on COM1: console output and input
    Framebuffer = 1u << 1,  // linear framebuffer text console
    PS2Keyboard = 1u << 2,  // PS/2 keyboard input
    RTC = 1u << 3,          // MC146818-compatible CMOS wall clock
    PCI = 1u << 4,          // PCI configuration-space enumeration
};
constexpr uint32_t kAll = Serial | Framebuffer | PS2Keyboard | RTC | PCI;
} // namespace Hw

constexpr uint32_t kHardwareMagic = 0x4645534Fu; // "OSEF" (little endian)
constexpr uint32_t kHardwareVersion = 1;
constexpr uint32_t kHardwareNameMax = 32;        // including the terminator

// Plain, fixed-layout record. Do not reorder: tools read it out of the ELF.
struct HardwareSpec {
    uint32_t magic;
    uint32_t version;
    uint32_t size;            // sizeof(HardwareSpec)
    uint32_t arch;            // Arch
    uint32_t firmware;        // Firmware
    uint32_t devices;         // Hw flags
    uint32_t memory_mb;       // minimum RAM the OS needs (also VM size for `run`)
    uint32_t serial_baud;     // COM1 baud rate
    uint32_t fb_width;        // preferred framebuffer mode; 0 = firmware default
    uint32_t fb_height;
    uint32_t boot_timeout_s;  // bootloader menu timeout; 0 = boot immediately
    uint32_t reserved;
    char name[kHardwareNameMax];
};
static_assert(sizeof(HardwareSpec) == 12 * 4 + kHardwareNameMax, "HardwareSpec layout changed");

const char* to_string(Arch arch);
const char* to_string(Firmware firmware);

namespace detail {
// Calling a non-constexpr function during constant evaluation is a compile
// error that names the function, which turns these into readable diagnostics.
void OSEF_HARDWARE_name_is_too_long_max_31_characters();
void OSEF_HARDWARE_name_must_not_be_empty();
void OSEF_HARDWARE_unknown_device_flag();
void OSEF_HARDWARE_baud_rate_must_divide_115200();
} // namespace detail

// Order-independent builder used inside OSEF_HARDWARE(...).
class Hardware {
public:
    constexpr Hardware() : spec_() {
        spec_.magic = kHardwareMagic;
        spec_.version = kHardwareVersion;
        spec_.size = sizeof(HardwareSpec);
        spec_.memory_mb = 128;
        spec_.serial_baud = 115200;
        const char def[] = "OSEF";
        for (uint32_t i = 0; i < sizeof(def); ++i) spec_.name[i] = def[i];
    }

    constexpr Hardware name(const char* n) const {
        Hardware h = *this;
        uint32_t i = 0;
        for (; n[i] != '\0'; ++i) {
            if (i + 1 >= kHardwareNameMax) detail::OSEF_HARDWARE_name_is_too_long_max_31_characters();
            h.spec_.name[i] = n[i];
        }
        if (i == 0) detail::OSEF_HARDWARE_name_must_not_be_empty();
        for (; i < kHardwareNameMax; ++i) h.spec_.name[i] = '\0';
        return h;
    }
    constexpr Hardware arch(Arch a) const {
        Hardware h = *this;
        h.spec_.arch = static_cast<uint32_t>(a);
        return h;
    }
    constexpr Hardware firmware(Firmware f) const {
        Hardware h = *this;
        h.spec_.firmware = static_cast<uint32_t>(f);
        return h;
    }
    constexpr Hardware devices(uint32_t flags) const {
        if (flags & ~Hw::kAll) detail::OSEF_HARDWARE_unknown_device_flag();
        Hardware h = *this;
        h.spec_.devices = flags;
        return h;
    }
    constexpr Hardware memory_mb(uint32_t mb) const {
        Hardware h = *this;
        h.spec_.memory_mb = mb;
        return h;
    }
    constexpr Hardware serial_baud(uint32_t baud) const {
        if (baud == 0 || 115200 % baud != 0) detail::OSEF_HARDWARE_baud_rate_must_divide_115200();
        Hardware h = *this;
        h.spec_.serial_baud = baud;
        return h;
    }
    constexpr Hardware resolution(uint32_t width, uint32_t height) const {
        Hardware h = *this;
        h.spec_.fb_width = width;
        h.spec_.fb_height = height;
        return h;
    }
    constexpr Hardware boot_timeout(uint32_t seconds) const {
        Hardware h = *this;
        h.spec_.boot_timeout_s = seconds;
        return h;
    }

    constexpr HardwareSpec spec() const { return spec_; }

private:
    HardwareSpec spec_;
};

// The kernel's view of the declaration (defined by OSEF_HARDWARE).
const HardwareSpec& hardware();

} // namespace osef

#if defined(OSEF_BAREMETAL)
#  define OSEF_HARDWARE_SECTION __attribute__((section(".osef_hardware"), aligned(16)))
#  define OSEF_HARDWARE_ATTR __attribute__((used, section(".osef_hardware"), aligned(16)))
#elif defined(__GNUC__) || defined(__clang__)
#  define OSEF_HARDWARE_SECTION
#  define OSEF_HARDWARE_ATTR __attribute__((used))
#else
#  define OSEF_HARDWARE_SECTION
#  define OSEF_HARDWARE_ATTR
#endif

extern "C" OSEF_HARDWARE_SECTION const osef::HardwareSpec osef_hardware_declaration;

// Declare the target hardware. Use exactly once, at file scope, at the top
// of the .cpp file that contains main().
#define OSEF_HARDWARE(...)                                                                   \
    namespace osef::detail {                                                                 \
    inline constexpr ::osef::HardwareSpec declared_hardware = (__VA_ARGS__).spec();          \
    }                                                                                        \
    static_assert(::osef::detail::declared_hardware.arch != 0,                               \
                  "OSEF_HARDWARE: choose a CPU architecture: .arch(osef::Arch::x86_64)");    \
    static_assert(::osef::detail::declared_hardware.arch ==                                  \
                      static_cast<uint32_t>(::osef::Arch::x86_64),                           \
                  "OSEF_HARDWARE: unsupported architecture (supported: x86_64)");            \
    static_assert(::osef::detail::declared_hardware.firmware != 0,                           \
                  "OSEF_HARDWARE: choose the boot firmware: "                                \
                  ".firmware(osef::Firmware::BIOS) or .firmware(osef::Firmware::UEFI)");     \
    static_assert(::osef::detail::declared_hardware.firmware <= 2,                           \
                  "OSEF_HARDWARE: firmware must be osef::Firmware::BIOS or ::UEFI");         \
    static_assert(::osef::detail::declared_hardware.devices &                                \
                      (::osef::Hw::Serial | ::osef::Hw::Framebuffer),                        \
                  "OSEF_HARDWARE: enable at least one console device: "                      \
                  ".devices(osef::Hw::Serial) and/or osef::Hw::Framebuffer");                \
    static_assert(::osef::detail::declared_hardware.memory_mb >= 32,                         \
                  "OSEF_HARDWARE: .memory_mb() must be at least 32");                        \
    static_assert((::osef::detail::declared_hardware.fb_width == 0) ==                       \
                      (::osef::detail::declared_hardware.fb_height == 0),                    \
                  "OSEF_HARDWARE: .resolution() needs both width and height");               \
    extern "C" OSEF_HARDWARE_ATTR const ::osef::HardwareSpec osef_hardware_declaration =     \
        ::osef::detail::declared_hardware
