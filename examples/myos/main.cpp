// MyOS: a tour of every extension point in OSEF.
//
//   hardware   -> OSEF_HARDWARE   (UEFI x86_64 machine; `hw` shows it)
//   module     -> MotdModule      (engine plugin: writes /etc, adds a device)
//   daemon     -> TimeService     (IPC service that answers requests)
//   programs   -> count, now, hello, crash
//   syscall    -> "answer"
//   scheduler  -> PriorityScheduler
//   storage    -> HostFS mounted at /disk (hosted builds; persists between runs)
//   init       -> custom pid 1 that starts the daemon, then the shell

#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("MyOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::UEFI)
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard | osef::Hw::RTC | osef::Hw::PCI | osef::Hw::USB)
    .memory_mb(128));

#include <osef/osef.h>

using namespace osef;

// ---------------------------------------------------------------- module
class MotdModule final : public Module {
public:
    const char* name() const override { return "motd"; }

    void on_load(Kernel& k) override {
        k.vfs().mkdir("/etc");
        k.vfs().write("/etc/motd", "Welcome to " + k.config().os_name +
                                       "! Type 'help' to get started.\n");
        // Modules can add hardware too: here's a write-only "led".
        k.register_device("led", {"pretend status LED", nullptr, [&k](StringView v) {
                                      k.print(String("[led] ") + (v.starts_with("1") ? "ON" : "off") + "\n");
                                      return true;
                                  }});
    }
};

// ---------------------------------------------------------------- daemon
class TimeService final : public Process {
public:
    void on_start(Context& c) override { c.register_service("time"); }

    void step(Context& c) override {
        Message msg;
        if (!c.receive(msg)) return; // sleeps until a message arrives
        c.send(msg.from, 1, to_string(c.kernel().uptime_ms()));
    }
};

// ---------------------------------------------------------------- programs
class Counter final : public Process {
public:
    void on_start(Context& c) override {
        if (!c.args().empty()) limit_ = static_cast<int>(to_int(c.args()[0], limit_));
    }

    void step(Context& c) override {
        c.println(format("count %d/%d", ++n_, limit_));
        if (n_ >= limit_) c.exit(0);
        else c.sleep(250); // other processes keep running meanwhile
    }

private:
    int n_ = 0;
    int limit_ = 5;
};

class AskTime final : public Process {
public:
    void step(Context& c) override {
        if (!asked_) {
            Pid service = c.lookup("time");
            if (!service) {
                c.println("now: time service is not running");
                c.exit(1);
                return;
            }
            c.send(service, 0);
            asked_ = true;
        }
        Message reply;
        if (!c.receive(reply)) return;
        c.println("time service says: " + reply.data + " ms since boot");
        c.exit(0);
    }

private:
    bool asked_ = false;
};

// ---------------------------------------------------------------- init
class Init final : public Process {
public:
    void on_start(Context& c) override {
        c.spawn("timed");
        c.wait(c.spawn("sh", {}, /*foreground=*/true));
    }
    // Runs once the shell exits.
    void step(Context& c) override { c.exit(c.last_child_exit()); }
};

int main() {
    KernelConfig config; // os_name defaults to the hardware name, "MyOS"
    config.version = "0.1.0";
    config.init_program = "init";

    Kernel kernel(config);
    kernel.set_scheduler(make_unique<PriorityScheduler>());
#if OSEF_HOSTED
    kernel.vfs().mount("/disk", make_shared<HostFS>("myos_disk"));
#endif

    kernel.add_module<ShellModule>();
    kernel.add_module<MotdModule>();

    auto& programs = kernel.programs();
    programs.add<Init>("init", "system init (pid 1)");
    programs.add<TimeService>("timed", "time daemon (IPC service 'time')");
    programs.add<Counter>("count", "count [n]: counts with sleeps between steps");
    programs.add<AskTime>("now", "asks the time daemon for the uptime");
    programs.add_fn("hello", "prints a greeting", [](Context& c) {
        c.println(format("Hello from %s, pid %u!", c.kernel().config().os_name.c_str(), c.pid()));
        c.exit(0);
    });
    programs.add_fn("crash", "crashes, to show crash isolation", [](Context& c) {
        c.abort("segfault (simulated)");
    });

    kernel.syscalls().define("answer", "the answer to everything",
                             [](Context&, const SyscallArgs&) { return SyscallResult{42, "life, the universe, everything"}; });

    return kernel.run();
}
