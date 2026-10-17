// BareMetalPlatform: the osef::Platform interface on top of the bare-metal
// x86_64 hardware layer (arch.h), plus the bare-metal runtime hooks
// osef::assert_fail and osef::create_default_platform().
//
// Input merges the PS/2 keyboard and COM1 (osef::bm::input_getc) and is
// line-edited here, with echo to every console, so the shell sees whole lines
// and interactive() is always true. Terminal escape sequences (arrow keys,
// Home/End, ... sent by the terminal on the serial line) are dropped.

#include <osef/platform.h>

#include "../escape_filter.h"
#include "arch.h"

namespace osef {

[[noreturn]] void assert_fail(const char* file, int line, const char* expr) {
    char msg[256];
    format_to(msg, sizeof msg, "assertion failed: %s (%s:%d)", expr ? expr : "?", file ? file : "?", line);
    bm::panic(msg);
}

namespace {

constexpr size_t kMaxLine = 1024;      // longer input lines are cut here
constexpr size_t kFaultMessageMax = 256;

class BareMetalPlatform final : public Platform {
public:
    const char* name() const override { return "x86_64"; }
    const char* firmware() const override { return bm::firmware_name(); }

    void init(const HardwareSpec&) override { partial_.reserve(128); }

    // The boot code powers the machine off once main() returns; nothing to do here.
    void shutdown(int) override {}

    uint64_t now_ms() override { return bm::ticks_ms(); }

    void sleep_ms(uint32_t ms) override {
        const uint64_t deadline = bm::ticks_ms() + ms;
        while (bm::ticks_ms() < deadline) bm::wait_for_interrupt();
    }

    void write(StringView text) override { bm::console_write(text.data(), text.size()); }
    void set_console_colors(int fg, int bg) override { bm::console_set_colors(fg, bg); }
    void set_console_scale(int scale) override { bm::console_set_scale(scale); }

    bool poll_line(String& out) override {
        char c;
        while (bm::input_getc(c)) {
            if (escapes_.swallow(c)) continue;  // arrow keys etc. over serial
            if (c == '\n') {
                bm::console_write("\n", 1);
                out = osef::move(partial_);
                partial_.clear();
                return true;
            }
            if (c == '\b') {
                if (!partial_.empty()) {
                    partial_.pop_back();
                    bm::console_write("\b \b", 3);
                }
                continue;
            }
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u > 0x7e) continue;  // other control characters (and non-ASCII bytes)
            if (partial_.size() >= kMaxLine) continue;
            partial_.push_back(c);
            bm::console_write(&c, 1);
        }
        return false;
    }

    bool input_closed() const override { return false; }
    bool interactive() const override { return true; }
    uint64_t random_u64() override { return bm::random_u64(); }

    bool read_wall_clock(WallClock& out) override {
        bm::RtcTime t{};
        if (!bm::rtc_read(t)) return false;
        out = {t.year, t.month, t.day, t.hour, t.minute, t.second};
        return true;
    }

    bool pci_devices(Vector<PciDevice>& out) override {
        bm::PciInfo found[256]{};
        const size_t count = bm::pci_scan(found, sizeof(found) / sizeof(found[0]));
        if (!(bm::spec().devices & Hw::PCI)) return false;
        const size_t stored = count < sizeof(found) / sizeof(found[0]) ? count : sizeof(found) / sizeof(found[0]);
        for (size_t i = 0; i < stored; ++i) {
            const bm::PciInfo& d = found[i];
            out.push_back({d.bus, d.device, d.function, d.vendor_id, d.device_id, d.class_code,
                           d.subclass, d.programming_interface});
        }
        return true;
    }

    MemoryStats memory() const override {
        const bm::HeapStats hs = bm::heap_stats();
        MemoryStats m;
        m.total = hs.total;
        m.used = hs.used;
        return m;
    }

    // CPU exceptions and fault() inside fn longjmp back to protected_call.
    // No destructors run on that path: whatever fn allocated is leaked.
    bool run_protected(void (*fn)(void*), void* arg, String& fault_message) override {
        char msg[kFaultMessageMax];
        if (bm::protected_call(fn, arg, msg, sizeof msg)) return true;
        fault_message = msg[0] ? StringView(msg) : StringView("fault");
        return false;
    }

    [[noreturn]] void fault(StringView message) override {
        char msg[kFaultMessageMax];
        const size_t n = message.size() < sizeof msg - 1 ? message.size() : sizeof msg - 1;
        for (size_t i = 0; i < n; ++i) msg[i] = message[i];
        msg[n] = '\0';
        bm::protected_fault(msg);
    }

private:
    String partial_;
    detail::EscapeFilter escapes_;
};

} // namespace

UniquePtr<Platform> create_default_platform() { return make_unique<BareMetalPlatform>(); }

} // namespace osef
