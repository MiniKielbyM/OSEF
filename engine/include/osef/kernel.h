#pragma once
// The Kernel is the engine runtime: it owns every subsystem, runs the main
// loop (like a game loop) and hands each process a Context, its API to the OS.

#include <osef/core.h>
#include <osef/hardware.h>
#include <osef/ipc.h>
#include <osef/module.h>
#include <osef/platform.h>
#include <osef/process.h>
#include <osef/program.h>
#include <osef/scheduler.h>
#include <osef/syscall.h>
#include <osef/vfs.h>

namespace osef {

enum class LogLevel { Debug = 0, Info, Warn, Error, None };

struct KernelConfig {
    String os_name;                       // empty: use the OSEF_HARDWARE .name()
    String version = "0.1.0";
    String init_program = "sh";           // first process (pid 1)
    Vector<String> init_args;
    uint32_t tick_hz = 100;               // main loop frequency
    uint32_t quanta_per_tick = 16;        // max process steps per tick
    LogLevel log_level = LogLevel::Info;
    bool show_banner = true;
    bool mount_devfs = true;              // mount built-in devices at /dev
};

struct SpawnOptions {
    Vector<String> args;
    Pid parent = kNoPid;
    int priority = 0;
    bool foreground = false;              // gets console input
    String cwd;                           // empty: inherit from parent
};

class Kernel;

// The API a process uses to talk to the OS. Valid only during the call it
// was passed to.
class Context {
public:
    Context(Kernel& kernel, Process& proc) : k_(kernel), p_(proc) {}

    Kernel& kernel() { return k_; }
    Pid pid() const { return p_.info_.pid; }
    const ProcessInfo& info() const { return p_.info_; }
    const Vector<String>& args() const { return p_.info_.args; }

    // Console
    void print(StringView text);
    void println(StringView text = {});
    // Non-blocking. Returns false and puts the process in WaitInput when no
    // line is available, and step() runs again once one arrives.
    bool read_line(String& out);
    bool input_eof() const;

    // Lifecycle
    void sleep(uint32_t ms);
    void exit(int code = 0);
    // Crash this process with `message` (the kernel logs it, kills the
    // process and keeps running). Use it where hosted code would throw.
    [[noreturn]] void abort(StringView message);
    Pid spawn(StringView program, Vector<String> args = {}, bool foreground = false);
    void wait(Pid child);                 // blocks until child exits
    int last_child_exit() const { return p_.info_.last_child_exit; }

    // IPC
    bool send(Pid to, uint32_t type, String data = {});
    bool receive(Message& out);           // non-blocking; blocks on empty
    bool register_service(StringView name);
    Pid lookup(StringView service) const;

    // Syscalls
    SyscallResult syscall(StringView name, const SyscallArgs& args = {});

    // Files (paths are relative to the process cwd)
    VFS& fs();
    const String& cwd() const { return p_.info_.cwd; }
    bool chdir(StringView path);
    String resolve(StringView path) const;
    bool read_file(StringView path, String& out);
    bool write_file(StringView path, StringView data, bool append = false);

private:
    bool block(ProcState state);
    Kernel& k_;
    Process& p_;
};

class Kernel {
public:
    explicit Kernel(KernelConfig config = {}, UniquePtr<Platform> platform = nullptr);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // ---- Composition: build your OS out of parts ----
    template <class T, class... Args>
    T& add_module(Args&&... args) {
        static_assert(is_base_of_v<Module, T>, "add_module<T>: T must derive from osef::Module");
        UniquePtr<T> mod = make_unique<T>(osef::forward<Args>(args)...);
        T& ref = *mod;
        ref.type_ = type_id<T>();
        attach_module(osef::move(mod));
        return ref;
    }
    // The module that was added as add_module<T>() (exact type match, no
    // RTTI needed), or nullptr.
    template <class T>
    T* find_module() {
        for (auto& m : modules_)
            if (m->type_ == type_id<T>()) return static_cast<T*>(m.get());
        return nullptr;
    }
    const Vector<UniquePtr<Module>>& modules() const { return modules_; }

    void set_scheduler(UniquePtr<Scheduler> scheduler);
    Scheduler& scheduler() { return *scheduler_; }
    void register_device(StringView name, Device dev) { devfs_->add(name, osef::move(dev)); }

    // ---- Subsystems ----
    const KernelConfig& config() const { return config_; }
    const HardwareSpec& hardware() const { return osef::hardware(); }
    Platform& platform() { return *platform_; }
    VFS& vfs() { return vfs_; }
    IPC& ipc() { return ipc_; }
    SyscallTable& syscalls() { return syscalls_; }
    ProgramRegistry& programs() { return programs_; }

    // ---- Processes ----
    Pid spawn(UniquePtr<Process> proc, String name, SpawnOptions opts = {});
    Pid exec(StringView program, SpawnOptions opts = {}); // kNoPid if unknown
    bool kill(Pid pid, int exit_code = -1);
    bool alive(Pid pid) const;
    const ProcessInfo* process_info(Pid pid) const;
    Vector<ProcessInfo> processes() const;
    Pid foreground() const { return foreground_; }
    void set_foreground(Pid pid) { foreground_ = pid; }
    bool send(Pid from, Pid to, uint32_t type, String data);

    // ---- I/O ----
    void print(StringView text) { platform_->write(text); }
    void push_input(String line) { input_.push_back(osef::move(line)); }
    void log(LogLevel level, StringView message);
    void panic(StringView message);

    // ---- Lifecycle ----
    int run();          // boot and loop until shutdown; returns exit code
    void boot();        // called automatically by run()/tick()
    bool tick();        // one main loop iteration; false once halted
    void shutdown(int exit_code = 0);
    bool running() const { return running_; }
    uint64_t uptime_ms() const;
    uint64_t ticks() const { return ticks_; }

private:
    friend class Context;

    void attach_module(UniquePtr<Module> mod);
    void register_builtins();
    void wake_processes();
    void run_quantum(Process& proc);
    void reap();
    void halt();
    Process* find(Pid pid) const;

    KernelConfig config_;
    UniquePtr<Platform> platform_;
    UniquePtr<Scheduler> scheduler_;
    SharedPtr<DevFS> devfs_;
    VFS vfs_;
    IPC ipc_;
    SyscallTable syscalls_;
    ProgramRegistry programs_;
    Vector<UniquePtr<Module>> modules_;

    Map<Pid, UniquePtr<Process>> procs_;
    Vector<Process*> ready_;
    Deque<String> input_;
    Pid next_pid_ = 1;
    Pid init_pid_ = kNoPid;
    Pid foreground_ = kNoPid;

    bool booted_ = false;
    bool running_ = false;
    bool halted_ = false;
    int exit_code_ = 0;
    uint64_t boot_time_ = 0;
    uint64_t run_counter_ = 0;
    uint64_t ticks_ = 0;
};

} // namespace osef
