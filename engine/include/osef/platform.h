#pragma once
// OSEF — Operating System Engine Framework
// Platform (HAL): everything the engine needs from the machine it runs on.
//
// The engine never touches the machine directly. To port OSEF to a new target
// (a GUI window, the browser, a microcontroller, bare metal) implement this
// interface and hand it to the Kernel constructor. Two implementations ship
// with the engine:
//   * hosted (engine/src/hosted/platform_host.cpp): Windows, macOS, Linux, web;
//   * bare metal (BareMetalPlatform): the x86_64 kernel booted by Limine.

#include <osef/core.h>
#include <osef/hardware.h>

#if OSEF_HAS_EXCEPTIONS
#  include <exception>
#endif

namespace osef {

struct MemoryStats {
    uint64_t total = 0; // bytes available to the OS; 0 = unknown
    uint64_t used = 0;  // bytes currently in use
};

struct WallClock {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

struct PciDevice {
    uint8_t bus = 0;
    uint8_t device = 0;
    uint8_t function = 0;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    uint8_t class_code = 0;
    uint8_t subclass = 0;
    uint8_t programming_interface = 0;
};

class Platform {
public:
    virtual ~Platform() = default;

    // "macos", "linux", "windows", "x86_64-uefi", ...
    virtual const char* name() const = 0;
    // How the machine booted: "bios", "uefi" or "hosted".
    virtual const char* firmware() const { return "hosted"; }

    // Called once at kernel boot with the OSEF_HARDWARE declaration.
    virtual void init(const HardwareSpec&) {}
    // "Kernel halted" hook, called from Kernel::halt with the exit code. It
    // must return: on bare metal the boot code powers the machine off once
    // main() returns.
    virtual void shutdown(int /*exit_code*/) {}

    // Monotonic milliseconds since init().
    virtual uint64_t now_ms() = 0;
    virtual void sleep_ms(uint32_t ms) = 0;

    // Console output.
    virtual void write(StringView text) = 0;
    // The console's default colors (ANSI color numbers 0-15; -1 = unchanged),
    // i.e. what "reset" returns to. Terminals can't change theirs; the bare-metal
    // framebuffer console can.
    virtual void set_console_colors(int /*fg*/, int /*bg*/) {}
    // Text size on graphical consoles (1-4 = font scale); terminals ignore it.
    virtual void set_console_scale(int /*scale*/) {}

    // Non-blocking line input. Returns true and fills `out` if a full line
    // is available. Must never block.
    virtual bool poll_line(String& out) = 0;
    // True once the input stream has ended and every line has been consumed.
    virtual bool input_closed() const = 0;
    // False when input is piped/scripted (the shell then echoes commands).
    virtual bool interactive() const { return true; }

    virtual uint64_t random_u64() = 0;

    // Optional wall-clock time and PCI enumeration. false means unavailable
    // on this platform or not enabled in the hardware declaration.
    virtual bool read_wall_clock(WallClock&) { return false; }
    virtual bool pci_devices(Vector<PciDevice>&) { return false; }

    // ---- Interrupts ----
    // Vectors 0-31 are CPU exceptions (reserved), 32-47 the legacy IRQ lines
    // 0-15 (vector = 32 + irq), 48-255 are free for the OS. claim_interrupt()
    // routes a vector to the kernel; take_interrupts() returns how many
    // arrived since the last call. The defaults simulate software interrupts
    // (raise_interrupt), so hosted builds and tests behave like bare metal;
    // only bare metal has real hardware IRQ lines.
    virtual bool hardware_interrupts() const { return false; }
    virtual bool claim_interrupt(uint8_t vector);
    virtual void release_interrupt(uint8_t vector);
    virtual uint32_t take_interrupts(uint8_t vector);
    virtual bool raise_interrupt(uint8_t vector);

    // Memory usage; total == 0 means the platform does not report it.
    virtual MemoryStats memory() const { return {}; }

    // Fault isolation: run fn(arg). If it faults (hosted: an exception; bare
    // metal: a CPU exception or fault()), return false with a message instead
    // of crashing. The default implementation has no isolation: it just calls
    // fn(arg) and returns true.
    virtual bool run_protected(void (*fn)(void*), void* arg, String& fault_message);
    // Abort the innermost run_protected region with `message`. Only valid
    // inside run_protected. The default implementation prints the message
    // and stops the machine (osef::assert_fail).
    [[noreturn]] virtual void fault(StringView message);

private:
    bool claimed_[256] = {};
    uint32_t pending_[256] = {};
};

#if OSEF_HAS_EXCEPTIONS
// Thrown by the hosted platform's fault() and caught by its run_protected().
class ProcessFault : public std::exception {
public:
    explicit ProcessFault(StringView message) : message_(message) {}
    const char* what() const noexcept override { return message_.c_str(); }
    StringView message() const noexcept { return message_; }

private:
    String message_;
};
#endif

// The platform the Kernel uses when none is given: the host platform in
// hosted builds, BareMetalPlatform in bare-metal builds.
UniquePtr<Platform> create_default_platform();

} // namespace osef
