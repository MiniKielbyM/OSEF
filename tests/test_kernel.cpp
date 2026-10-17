// Engine tests. They drive the kernel through a scripted Platform with a
// virtual clock: deterministic, instant, and no terminal needed.

#include <osef/hardware.h>

// The test binary is an OS too, so it declares its hardware like any other.
OSEF_HARDWARE(osef::Hardware()
    .name("TestOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::BIOS)
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer)
    .memory_mb(64)
    .serial_baud(38400)
    .resolution(1024, 768));

#include <osef/osef.h>

#include <cstdio>
#include <exception>
#include <stdexcept>

using namespace osef;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

class ScriptedPlatform final : public Platform {
public:
    ScriptedPlatform(Deque<String> lines, String* out) : lines_(osef::move(lines)), out_(out) {}
    const char* name() const override { return "test"; }
    uint64_t now_ms() override { return clock_; }
    void sleep_ms(uint32_t ms) override { clock_ += ms; }
    void write(StringView text) override { *out_ += text; }
    bool poll_line(String& line) override {
        if (lines_.empty()) return false;
        line = osef::move(lines_.front());
        lines_.pop_front();
        return true;
    }
    bool input_closed() const override { return lines_.empty(); }
    bool interactive() const override { return false; }
    uint64_t random_u64() override { return 4; } // chosen by fair dice roll
    MemoryStats memory() const override { return memory_; }
    void shutdown(int exit_code) override {
        shutdown_calls_++;
        shutdown_code_ = exit_code;
    }

    // Same isolation strategy as the host platform: fault() throws.
    bool run_protected(void (*fn)(void*), void* arg, String& fault_message) override {
        try {
            fn(arg);
            return true;
        } catch (const ProcessFault& f) {
            fault_message = f.message();
        } catch (const std::exception& e) {
            fault_message = e.what();
        }
        return false;
    }
    [[noreturn]] void fault(StringView message) override { throw ProcessFault(message); }

    void set_console_colors(int fg, int bg) override {
        console_fg = fg;
        console_bg = bg;
    }
    void set_console_scale(int scale) override { console_scale = scale; }
    int console_fg = -2, console_bg = -2, console_scale = 0;

    void advance(uint64_t ms) { clock_ += ms; }
    void set_memory(MemoryStats m) { memory_ = m; }
    int shutdown_calls() const { return shutdown_calls_; }
    int shutdown_code() const { return shutdown_code_; }

private:
    Deque<String> lines_;
    String* out_;
    uint64_t clock_ = 0;
    MemoryStats memory_;
    int shutdown_calls_ = 0;
    int shutdown_code_ = 0;
};

struct Harness {
    String out;
    ScriptedPlatform* platform = nullptr;
    UniquePtr<Kernel> kernel;

    explicit Harness(Deque<String> script, KernelConfig cfg = {}, bool banner = false) {
        cfg.show_banner = banner;
        auto p = make_unique<ScriptedPlatform>(osef::move(script), &out);
        platform = p.get();
        kernel = make_unique<Kernel>(osef::move(cfg), osef::move(p));
    }

    // Run until halt (or a tick limit, so a bug can't hang the test).
    int run(int max_ticks = 10000) {
        for (int i = 0; i < max_ticks; ++i) {
            if (!kernel->tick()) return i;
            platform->advance(10);
        }
        return -1;
    }

    bool has(StringView s) const { return out.find(s) != String::npos; }
};

static void test_paths() {
    CHECK(path::normalize("/", "a/b/../c") == "/a/c");
    CHECK(path::normalize("/x/y", "..") == "/x");
    CHECK(path::normalize("/x", "/../..//z/.") == "/z");
    CHECK(path::normalize("/", "") == "/");
    CHECK(path::parent("/a/b") == "/a");
    CHECK(path::parent("/a") == "/");
    CHECK(path::basename("/a/b") == "b");
}

static void test_shell_and_vfs() {
    Harness h({"mkdir /tmp", "echo hello world > /tmp/a.txt", "echo again >> /tmp/a.txt",
               "cat /tmp/a.txt", "cd /tmp", "pwd", "ls", "ls /", "echo ping > /dev/console", "nope"});
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(h.has("hello world\nagain\n"));
    CHECK(h.has("/tmp\n"));
    CHECK(h.has("a.txt"));
    CHECK(h.has("dev/"));
    CHECK(h.has("ping\n"));
    CHECK(h.has("command not found: nope"));
    CHECK(h.has("system halted"));
}

static void test_programs_sleep_and_wait() {
    Harness h({"count", "echo done"});
    h.kernel->add_module<ShellModule>();
    int n = 0;
    h.kernel->programs().add_fn("count", "", [n](Context& c) mutable {
        c.println("count " + to_string(++n));
        if (n == 3) c.exit(7);
        else c.sleep(100);
    });
    CHECK(h.run() >= 0);
    size_t c3 = h.out.find("count 3");
    size_t done = h.out.find("done\n");
    CHECK(c3 != String::npos);
    CHECK(done != String::npos && done > c3); // shell waited for the child
}

static void test_ipc_service() {
    Harness h({"client"});
    h.kernel->add_module<ShellModule>();
    h.kernel->programs().add_fn("echo-service", "", [](Context& c) {
        if (c.info().steps == 1) c.register_service("echo");
        Message m;
        if (c.receive(m)) c.send(m.from, m.type + 1, "re:" + m.data);
    });
    bool sent = false;
    h.kernel->programs().add_fn("client", "", [sent](Context& c) mutable {
        if (!sent) {
            CHECK(c.lookup("echo") != kNoPid);
            c.send(c.lookup("echo"), 10, "hi");
            sent = true;
        }
        Message m;
        if (!c.receive(m)) return;
        c.println("got " + to_string(m.type) + " " + m.data);
        c.exit(0);
    });
    // Start the service before the shell runs anything.
    h.kernel->boot();
    h.kernel->exec("echo-service");
    CHECK(h.run() >= 0);
    CHECK(h.has("got 11 re:hi"));
}

static void test_crash_isolation() {
    Harness h({"boom", "echo still alive"});
    h.kernel->add_module<ShellModule>();
    h.kernel->programs().add_fn("boom", "", [](Context&) { throw std::runtime_error("kaboom"); });
    CHECK(h.run() >= 0);
    CHECK(h.has("crashed: kaboom"));
    CHECK(h.has("still alive\n"));
}

static void test_abort_isolation() {
    // Context::abort is the freestanding way to crash a process: it goes
    // through Platform::fault, which unwinds to the kernel's run_protected.
    Harness h({"bad", "ps", "echo still alive"});
    h.kernel->add_module<ShellModule>();
    bool after_abort = false;
    h.kernel->programs().add_fn("bad", "", [&after_abort](Context& c) {
        c.abort("segfault (simulated)");
        after_abort = true; // unreachable
    });
    CHECK(h.run() >= 0);
    CHECK(h.has("pid 2 (bad) crashed: segfault (simulated)"));
    CHECK(!after_abort);
    CHECK(h.has("still alive\n"));
    CHECK(h.out.find("  bad\n") == String::npos); // ps: the crashed process was reaped
    CHECK(h.platform->shutdown_calls() == 1);    // halt hook ran exactly once
}

static void test_shutdown_code_and_syscalls() {
    Harness h({"sys getpid", "sys answer", "sys missing", "shutdown 3"});
    h.kernel->add_module<ShellModule>();
    h.kernel->syscalls().define("answer", "", [](Context&, const SyscallArgs&) {
        return SyscallResult{42, "yes"};
    });
    CHECK(h.run() >= 0);
    CHECK(h.has("1\n"));       // shell is pid 1
    CHECK(h.has("42  yes"));
    CHECK(h.has("-38"));       // ENOSYS
    CHECK(h.kernel->run() == 3);
    CHECK(h.platform->shutdown_code() == 3);
}

static void test_missing_init_panics() {
    Harness h({});
    CHECK(h.run() >= 0);
    CHECK(h.has("KERNEL PANIC"));
}

static void test_hw_builtin() {
    Harness h({"hw"});
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(h.has("name:       TestOS\n"));
    CHECK(h.has("arch:       x86_64\n"));
    CHECK(h.has("firmware:   BIOS\n"));
    CHECK(h.has("devices:    serial framebuffer\n"));
    CHECK(h.has("memory:     64 MiB\n"));
    CHECK(h.has("serial:     38400 baud\n"));
    CHECK(h.has("resolution: 1024x768\n"));
    CHECK(hardware().memory_mb == 64);
    CHECK(StringView(to_string(Firmware::UEFI)) == "UEFI");
    CHECK(StringView(to_string(Arch::x86_64)) == "x86_64");
}

static void test_mem_builtin() {
    {
        Harness h({"mem"});
        h.kernel->add_module<ShellModule>();
        CHECK(h.run() >= 0);
        CHECK(h.has("memory: n/a"));
    }
    {
        Harness h({"mem"});
        h.kernel->add_module<ShellModule>();
        h.platform->set_memory({64ull << 20, 16ull << 20});
        CHECK(h.run() >= 0);
        CHECK(h.has("total      65536 KiB      64 MiB\n"));
        CHECK(h.has("used       16384 KiB      16 MiB\n"));
        CHECK(h.has("free       49152 KiB      48 MiB\n"));
    }
}

namespace {
class AlphaModule final : public Module {
public:
    const char* name() const override { return "alpha"; }
    int value = 1;
};
class BetaModule final : public Module {
public:
    explicit BetaModule(int v) : value(v) {}
    const char* name() const override { return "beta"; }
    int value;
};
class UnusedModule final : public Module {
public:
    const char* name() const override { return "unused"; }
};
} // namespace

static void test_find_module_type_tags() {
    Harness h({"mods"});
    h.kernel->add_module<ShellModule>();
    AlphaModule& alpha = h.kernel->add_module<AlphaModule>();
    BetaModule& beta = h.kernel->add_module<BetaModule>(42);
    CHECK(h.kernel->find_module<AlphaModule>() == &alpha);
    CHECK(h.kernel->find_module<BetaModule>() == &beta);
    CHECK(h.kernel->find_module<BetaModule>()->value == 42);
    CHECK(h.kernel->find_module<const AlphaModule>() == &alpha);
    CHECK(h.kernel->find_module<ShellModule>() != nullptr);
    CHECK(h.kernel->find_module<UnusedModule>() == nullptr);
    CHECK(alpha.type() == type_id<AlphaModule>());
    CHECK(type_id<AlphaModule>() != type_id<BetaModule>());
    CHECK(h.run() >= 0);
    CHECK(h.has("shell\nalpha\nbeta\n"));
}

static void test_os_name_falls_back_to_hardware() {
    {
        Harness h({"uname"}, KernelConfig{}, /*banner=*/true);
        h.kernel->add_module<ShellModule>();
        CHECK(h.kernel->config().os_name == "TestOS");
        CHECK(h.run() >= 0);
        CHECK(h.has("TestOS 0.1.0\x1b[0m  (OSEF engine on test, hosted boot)\n"));
        CHECK(h.has("TestOS 0.1.0 test hosted\n"));
    }
    {
        KernelConfig cfg;
        cfg.os_name = "Custom";
        Harness h({}, cfg);
        CHECK(h.kernel->config().os_name == "Custom");
    }
}

static KernelConfig init_config() {
    KernelConfig cfg;
    cfg.init_program = "init";
    return cfg;
}

static void test_round_robin_fairness() {
    Harness h({}, init_config());
    int a = 0, b = 0;
    h.kernel->programs().add_fn("init", "", [](Context& c) {
        if (c.info().steps == 1) {
            c.spawn("a");
            c.spawn("b");
            c.sleep(200);
            return;
        }
        c.kernel().shutdown(0);
    });
    h.kernel->programs().add_fn("a", "", [&a](Context&) { ++a; });
    h.kernel->programs().add_fn("b", "", [&b](Context&) { ++b; });
    CHECK(h.run() >= 0);
    CHECK(a > 100);
    CHECK(a - b <= 1 && b - a <= 1);
}

static void test_priority_order() {
    // High-priority process runs before low-priority one.
    Harness h({}, init_config());
    String order;
    h.kernel->set_scheduler(make_unique<PriorityScheduler>());
    h.kernel->programs().add_fn("init", "", [](Context& c) {
        if (c.info().steps == 1) {
            SpawnOptions lo;
            lo.priority = 1;
            lo.parent = c.pid();
            SpawnOptions hi = lo;
            hi.priority = 9;
            c.kernel().exec("lo", lo);
            c.kernel().exec("hi", hi);
            c.sleep(50);
            return;
        }
        c.exit(0);
    });
    h.kernel->programs().add_fn("lo", "", [&order](Context& c) { order += "L"; c.exit(); });
    h.kernel->programs().add_fn("hi", "", [&order](Context& c) { order += "H"; c.exit(); });
    CHECK(h.run() >= 0);
    CHECK(order == "HL");
}

static void test_lottery_scheduler() {
    // Every ready process wins some draws; more tickets win more often.
    Harness h({}, init_config());
    int lo = 0, hi = 0;
    h.kernel->set_scheduler(make_unique<LotteryScheduler>(1234));
    h.kernel->programs().add_fn("init", "", [](Context& c) {
        if (c.info().steps == 1) {
            SpawnOptions o;
            o.parent = c.pid();
            o.priority = 0;
            c.kernel().exec("lo", o);
            o.priority = 3;
            c.kernel().exec("hi", o);
            c.sleep(500);
            return;
        }
        c.kernel().shutdown(0);
    });
    h.kernel->programs().add_fn("lo", "", [&lo](Context&) { ++lo; });
    h.kernel->programs().add_fn("hi", "", [&hi](Context&) { ++hi; });
    CHECK(h.run() >= 0);
    CHECK(StringView(h.kernel->scheduler().name()) == "lottery");
    CHECK(lo > 0);
    CHECK(hi > 2 * lo);
}


// Appearance: welcome text, prompt template + colors, console defaults.
static void test_appearance() {
    KernelConfig cfg;
    cfg.appearance.welcome = "Hi there";
    cfg.appearance.prompt = "<{os}@{cwd}> ";
    cfg.appearance.text = ConsoleColor::BrightYellow;
    cfg.appearance.background = ConsoleColor::Blue;
    cfg.appearance.prompt_name = ConsoleColor::BrightRed;
    cfg.appearance.prompt_path = ConsoleColor::Default;
    cfg.appearance.text_size = 2;
    Harness h({"exit"}, osef::move(cfg));
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(h.has("Hi there\n"));
    const String reset = "\x1b[0m\x1b[93m\x1b[44m";
    CHECK(h.kernel->style_reset() == reset);
    CHECK(h.has("<\x1b[1m\x1b[91mTestOS" + reset + "@/> "));  // {cwd} uncolored (Default)
    CHECK(h.has(reset + "\x1b[2J\x1b[H"));                     // background clears the screen
    CHECK(h.platform->console_fg == 11);
    CHECK(h.platform->console_bg == 4);
    CHECK(h.platform->console_scale == 2);
    CHECK(Kernel::color_code(ConsoleColor::Default).empty());
    CHECK(Kernel::color_code(ConsoleColor::Green) == "\x1b[32m");
    CHECK(Kernel::color_code(ConsoleColor::BrightBlue, true) == "\x1b[104m");
    CHECK(StringView(to_string(ConsoleColor::BrightCyan)) == "bright-cyan");
}

// The default appearance keeps the classic prompt and leaves the console alone.
static void test_default_appearance() {
    Harness h({"exit"});
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(h.has("\x1b[1m\x1b[92mTestOS\x1b[0m:\x1b[1m\x1b[94m/\x1b[0m$ "));
    CHECK(!h.has("\x1b[2J"));
    CHECK(h.platform->console_scale == 0);
    CHECK(h.platform->console_fg == -1 && h.platform->console_bg == -1);
}

// Modules added before boot can configure the kernel (on_configure).
struct ConfigureModule final : Module {
    const char* name() const override { return "configure"; }
    void on_configure(KernelConfig& config) override {
        config.os_name = "Configured";
        config.appearance.welcome = "configured welcome";
    }
};

static void test_on_configure() {
    Harness h({"uname", "exit"}, {}, /*banner=*/true);
    h.kernel->add_module<ConfigureModule>();
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(h.has("Configured 0.1.0"));
    CHECK(h.has("configured welcome"));
}

// OSEF_AUTO_MODULE: every Kernel gets the module without a line in main().
static int g_auto_loads = 0;
struct AutoTestModule final : Module {
    const char* name() const override { return "auto-test"; }
    void on_load(Kernel&) override { ++g_auto_loads; }
};
OSEF_AUTO_MODULE(AutoTestModule);

static void test_auto_module() {
    const int before = g_auto_loads;
    Harness h({"exit"});
    CHECK(h.kernel->find_module<AutoTestModule>() != nullptr);
    CHECK(h.kernel->modules().size() >= 1 && h.kernel->modules()[0]->type() == type_id<AutoTestModule>());
    h.kernel->add_module<ShellModule>();
    CHECK(h.run() >= 0);
    CHECK(g_auto_loads == before + 1);
    bool found = false;
    for (const AutoModule* m = auto_modules(); m; m = m->next)
        if (m->type == type_id<AutoTestModule>()) found = true;
    CHECK(found);
}


// Custom interrupts: soft vectors are counted, delivered on the next tick,
// isolated when they fault, and listed by the `irq` builtin.
static void test_interrupts() {
    Harness h({"int 0x80", "int 0x80", "int 20", "int 0x81", "irq", "exit"});
    h.kernel->add_module<ShellModule>();
    Interrupts& irq = h.kernel->interrupts();
    uint32_t seen = 0;
    int calls = 0;
    CHECK(irq.on_vector(0x80, "test-call", [&](Kernel& k, uint8_t v, uint32_t n) {
        CHECK(v == 0x80);
        seen += n;
        ++calls;
        k.print("handled 0x80\n");
    }));
    CHECK(irq.on_vector(0x81, "boom", [](Kernel&, uint8_t, uint32_t) { throw std::runtime_error("bad handler"); }));
    CHECK(!irq.on_vector(20, "exception", [](Kernel&, uint8_t, uint32_t) {}));  // CPU exception vectors reserved
    CHECK(!irq.on_vector(40, "irq-range", [](Kernel&, uint8_t, uint32_t) {}));  // IRQ lines use on_irq
    CHECK(irq.on_irq(12, "mouse", [](Kernel&, uint8_t, uint32_t) {}));
    CHECK(!irq.on_irq(16, "bad", [](Kernel&, uint8_t, uint32_t) {}));
    CHECK(!irq.raise(0x2c));     // IRQ vectors cannot be raised in software
    CHECK(!irq.hardware());      // hosted/test platform: simulated
    CHECK(h.run() >= 0);
    CHECK(seen == 2);
    CHECK(calls >= 1);
    CHECK(irq.count(0x80) == 2);
    CHECK(h.has("handled 0x80"));
    CHECK(h.has("int: vector 0x14 is reserved"));
    CHECK(h.has("interrupt 0x81 (boom) handler crashed: bad handler"));
    CHECK(h.has("  0x80  soft         2  test-call"));
    CHECK(h.has("  0x2c  IRQ 12       0  mouse"));
    CHECK(irq.remove(0x80));
    CHECK(!irq.remove(0x80));
    CHECK(irq.list().size() == 2);
}

int main() {
    struct {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"interrupts", test_interrupts},
        {"appearance", test_appearance},
        {"default_appearance", test_default_appearance},
        {"on_configure", test_on_configure},
        {"auto_module", test_auto_module},
        {"paths", test_paths},
        {"shell_and_vfs", test_shell_and_vfs},
        {"programs_sleep_and_wait", test_programs_sleep_and_wait},
        {"ipc_service", test_ipc_service},
        {"crash_isolation", test_crash_isolation},
        {"abort_isolation", test_abort_isolation},
        {"shutdown_code_and_syscalls", test_shutdown_code_and_syscalls},
        {"missing_init_panics", test_missing_init_panics},
        {"hw_builtin", test_hw_builtin},
        {"mem_builtin", test_mem_builtin},
        {"find_module_type_tags", test_find_module_type_tags},
        {"os_name_falls_back_to_hardware", test_os_name_falls_back_to_hardware},
        {"round_robin_fairness", test_round_robin_fairness},
        {"priority_order", test_priority_order},
        {"lottery_scheduler", test_lottery_scheduler},
    };
    for (auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "[ ok ]" : "[FAIL]", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
