#include <osef/kernel.h>

#if defined(__EMSCRIPTEN__)
#  include <emscripten.h>
#endif

namespace osef {

const char* to_string(ProcState state) {
    switch (state) {
        case ProcState::Ready: return "ready";
        case ProcState::Sleeping: return "sleeping";
        case ProcState::WaitInput: return "wait-input";
        case ProcState::WaitMessage: return "wait-msg";
        case ProcState::WaitChild: return "wait-child";
        case ProcState::Zombie: return "zombie";
    }
    return "?";
}

// ================================================================ Context

void Context::print(StringView text) { k_.print(text); }

void Context::println(StringView text) {
    k_.print(text);
    k_.print("\n");
}

bool Context::block(ProcState state) {
    if (p_.info_.state == ProcState::Zombie) return false;
    p_.info_.state = state;
    return true;
}

bool Context::read_line(String& out) {
    if (k_.foreground_ == pid() && !k_.input_.empty()) {
        out = osef::move(k_.input_.front());
        k_.input_.pop_front();
        return true;
    }
    if (!input_eof()) block(ProcState::WaitInput);
    return false;
}

bool Context::input_eof() const { return k_.input_.empty() && k_.platform_->input_closed(); }

void Context::sleep(uint32_t ms) {
    if (block(ProcState::Sleeping)) p_.info_.wake_at = k_.uptime_ms() + ms;
}

void Context::exit(int code) { k_.kill(pid(), code); }

void Context::abort(StringView message) { k_.platform_->fault(message); }

Pid Context::spawn(StringView program, Vector<String> args, bool foreground) {
    SpawnOptions opts;
    opts.args = osef::move(args);
    opts.parent = pid();
    opts.foreground = foreground;
    return k_.exec(program, osef::move(opts));
}

void Context::wait(Pid child) {
    if (!k_.alive(child) || child == pid()) return;
    if (block(ProcState::WaitChild)) p_.info_.waiting_for = child;
}

bool Context::send(Pid to, uint32_t type, String data) { return k_.send(pid(), to, type, osef::move(data)); }

bool Context::receive(Message& out) {
    if (k_.ipc_.take(pid(), out)) return true;
    block(ProcState::WaitMessage);
    return false;
}

bool Context::register_service(StringView name) { return k_.ipc_.register_service(name, pid()); }
Pid Context::lookup(StringView service) const { return k_.ipc_.lookup(service); }

SyscallResult Context::syscall(StringView name, const SyscallArgs& args) {
    return k_.syscalls_.invoke(*this, name, args);
}

VFS& Context::fs() { return k_.vfs_; }

String Context::resolve(StringView p) const { return path::normalize(p_.info_.cwd, p); }

bool Context::chdir(StringView p) {
    String target = resolve(p);
    if (k_.vfs_.stat(target).type != NodeType::Directory) return false;
    p_.info_.cwd = osef::move(target);
    return true;
}

bool Context::read_file(StringView p, String& out) { return k_.vfs_.read(resolve(p), out); }

bool Context::write_file(StringView p, StringView data, bool append) {
    return k_.vfs_.write(resolve(p), data, append);
}

// ================================================================ Kernel

Kernel::Kernel(KernelConfig config, UniquePtr<Platform> platform)
    : config_(osef::move(config)),
      platform_(platform ? osef::move(platform) : create_default_platform()),
      scheduler_(make_unique<RoundRobinScheduler>()),
      devfs_(make_shared<DevFS>()) {
    if (config_.os_name.empty()) config_.os_name = hardware().name;
    if (config_.tick_hz == 0) config_.tick_hz = 1;
    if (config_.quanta_per_tick == 0) config_.quanta_per_tick = 1;
    register_builtins();
}

Kernel::~Kernel() {
    if (booted_ && !halted_) halt();
}

void Kernel::register_builtins() {
    // ---- devices (/dev) ----
    register_device("null", {"discards writes, reads empty",
                             [](String& out) {
                                 out.clear();
                                 return true;
                             },
                             [](StringView) { return true; }});
    register_device("console", {"write to the system console", nullptr, [this](StringView data) {
                                    print(data);
                                    return true;
                                }});
    register_device("random", {"64 random bits as hex",
                               [this](String& out) {
                                   out = format("%016llx\n", static_cast<unsigned long long>(platform_->random_u64()));
                                   return true;
                               },
                               nullptr});
    register_device("uptime", {"milliseconds since boot",
                               [this](String& out) {
                                   out = to_string(static_cast<unsigned long long>(uptime_ms())) + "\n";
                                   return true;
                               },
                               nullptr});

    // ---- syscalls ----
    syscalls_.define("getpid", "current process id",
                     [](Context& c, const SyscallArgs&) { return SyscallResult{c.pid(), {}}; });
    syscalls_.define("getppid", "parent process id",
                     [](Context& c, const SyscallArgs&) { return SyscallResult{c.info().parent, {}}; });
    syscalls_.define("uptime", "milliseconds since boot", [this](Context&, const SyscallArgs&) {
        return SyscallResult{static_cast<int64_t>(uptime_ms()), {}};
    });
    syscalls_.define("uname", "os name, version, platform and firmware", [this](Context&, const SyscallArgs&) {
        return SyscallResult{0, config_.os_name + " " + config_.version + " " + platform_->name() + " " +
                                    platform_->firmware()};
    });
    syscalls_.define("kill", "kill <pid> [code]", [this](Context&, const SyscallArgs& a) {
        if (a.empty()) return SyscallResult{-22, "usage: kill <pid> [code]"};
        long long pid = 0;
        long long code = -1;
        if (!parse_int(a[0], pid) || pid <= 0 || pid > 0xFFFFFFFFll || (a.size() > 1 && !parse_int(a[1], code)))
            return SyscallResult{-22, "invalid argument"};
        return kill(static_cast<Pid>(pid), static_cast<int>(code)) ? SyscallResult{0, {}}
                                                                   : SyscallResult{-3, "no such process"};
    });
    syscalls_.define("spawn", "spawn <program> [args...]", [](Context& c, const SyscallArgs& a) {
        if (a.empty()) return SyscallResult{-22, "usage: spawn <program> [args...]"};
        Pid pid = c.spawn(a[0], SyscallArgs(a.begin() + 1, a.end()));
        return pid ? SyscallResult{pid, {}} : SyscallResult{-2, "no such program"};
    });
}

void Kernel::attach_module(UniquePtr<Module> mod) {
    Module& ref = *mod;
    modules_.push_back(osef::move(mod));
    // Modules added before boot are loaded during boot.
    if (booted_ && !halted_) {
        log(LogLevel::Debug, String("module loaded: ") + ref.name());
        ref.on_load(*this);
    }
}

void Kernel::set_scheduler(UniquePtr<Scheduler> scheduler) {
    if (scheduler) scheduler_ = osef::move(scheduler);
}

Process* Kernel::find(Pid pid) const {
    auto it = procs_.find(pid);
    return it == procs_.end() ? nullptr : it->second.get();
}

Pid Kernel::spawn(UniquePtr<Process> proc, String name, SpawnOptions opts) {
    if (!proc) return kNoPid;
    Pid pid = next_pid_++;
    ProcessInfo& info = proc->info_;
    info = ProcessInfo{};
    info.pid = pid;
    info.parent = opts.parent;
    info.name = osef::move(name);
    info.args = osef::move(opts.args);
    info.priority = opts.priority;
    if (!opts.cwd.empty()) info.cwd = path::normalize("/", opts.cwd);
    else if (Process* parent = find(opts.parent)) info.cwd = parent->info_.cwd;
    if (opts.foreground) foreground_ = pid;
    log(LogLevel::Debug, format("spawn pid %u (%s)", pid, info.name.c_str()));
    procs_.insert_or_assign(pid, osef::move(proc));
    return pid;
}

Pid Kernel::exec(StringView program, SpawnOptions opts) {
    const Program* prog = programs_.find(program);
    if (!prog) return kNoPid;
    return spawn(prog->factory(), String(program), osef::move(opts));
}

bool Kernel::kill(Pid pid, int exit_code) {
    Process* p = find(pid);
    if (!p || p->info_.state == ProcState::Zombie) return false;
    p->info_.state = ProcState::Zombie;
    p->info_.exit_code = exit_code;
    return true;
}

bool Kernel::alive(Pid pid) const {
    Process* p = find(pid);
    return p && p->info_.state != ProcState::Zombie;
}

const ProcessInfo* Kernel::process_info(Pid pid) const {
    Process* p = find(pid);
    return p ? &p->info_ : nullptr;
}

Vector<ProcessInfo> Kernel::processes() const {
    Vector<ProcessInfo> out;
    out.reserve(procs_.size());
    for (const auto& [pid, p] : procs_) out.push_back(p->info_);
    return out;
}

bool Kernel::send(Pid from, Pid to, uint32_t type, String data) {
    Process* target = find(to);
    if (!target || target->info_.state == ProcState::Zombie) return false;
    ipc_.post(to, Message{from, type, osef::move(data)});
    if (target->info_.state == ProcState::WaitMessage) target->info_.state = ProcState::Ready;
    return true;
}

void Kernel::log(LogLevel level, StringView message) {
    if (level < config_.log_level || level == LogLevel::None) return;
    static const char* const tags[] = {"\x1b[90mdebug\x1b[0m", "\x1b[36minfo\x1b[0m", "\x1b[33mwarn\x1b[0m",
                                       "\x1b[31merror\x1b[0m"};
    uint64_t t = booted_ ? uptime_ms() : 0;
    String line = format("[%5llu.%03llu] %s: ", static_cast<unsigned long long>(t / 1000),
                         static_cast<unsigned long long>(t % 1000), tags[static_cast<int>(level)]);
    line += message;
    line += '\n';
    print(line);
}

void Kernel::panic(StringView message) {
    log(LogLevel::Error, "KERNEL PANIC: " + message);
    shutdown(1);
}

uint64_t Kernel::uptime_ms() const { return platform_->now_ms() - boot_time_; }

void Kernel::shutdown(int exit_code) {
    exit_code_ = exit_code;
    running_ = false;
}

// ---------------------------------------------------------------- lifecycle

void Kernel::boot() {
    if (booted_) return;
    platform_->init(hardware());
    boot_time_ = platform_->now_ms();
    running_ = true;

    if (config_.show_banner) {
        print("\x1b[1m" + config_.os_name + " " + config_.version + "\x1b[0m  (OSEF engine on " + platform_->name() +
              ", " + platform_->firmware() + " boot)\n");
    }

    if (!vfs_.has_mount("/")) vfs_.mount("/", make_shared<RamFS>());
    if (config_.mount_devfs && !vfs_.has_mount("/dev")) vfs_.mount("/dev", devfs_);
    log(LogLevel::Debug, String("scheduler: ") + scheduler_->name());

    // Modules may add more modules from on_load, so iterate by index.
    for (size_t i = 0; i < modules_.size(); ++i) {
        Module* m = modules_[i].get();
        log(LogLevel::Debug, String("module loaded: ") + m->name());
        m->on_load(*this);
    }
    booted_ = true;

    SpawnOptions opts;
    opts.args = config_.init_args;
    opts.foreground = true;
    init_pid_ = exec(config_.init_program, osef::move(opts));
    if (init_pid_ == kNoPid) panic("init program '" + config_.init_program + "' is not registered");
}

bool Kernel::tick() {
    boot();
    if (halted_) return false;

    if (running_) {
        String line;
        while (platform_->poll_line(line)) input_.push_back(osef::move(line));

        wake_processes();
        for (size_t i = 0; i < modules_.size() && running_; ++i) modules_[i]->on_tick(*this);

        for (uint32_t q = 0; q < config_.quanta_per_tick && running_; ++q) {
            ready_.clear();
            for (auto& [pid, p] : procs_)
                if (p->info_.state == ProcState::Ready) ready_.push_back(p.get());
            if (ready_.empty()) break;
            run_quantum(*scheduler_->pick(ready_));
        }
        reap();
        ++ticks_;
    }

    if (!running_) {
        halt();
        return false;
    }
    return true;
}

int Kernel::run() {
    boot();
#if defined(__EMSCRIPTEN__)
    // The browser owns the loop; we get called once per frame.
    emscripten_set_main_loop_arg(
        [](void* self) {
            if (!static_cast<Kernel*>(self)->tick()) emscripten_cancel_main_loop();
        },
        this, static_cast<int>(config_.tick_hz), 1);
    return exit_code_;
#else
    const uint64_t period = max<uint64_t>(1, 1000 / config_.tick_hz);
    uint64_t next = platform_->now_ms();
    while (tick()) {
        next += period;
        uint64_t now = platform_->now_ms();
        if (next > now) platform_->sleep_ms(static_cast<uint32_t>(next - now));
        else next = now; // fell behind; don't try to catch up
    }
    return exit_code_;
#endif
}

void Kernel::wake_processes() {
    const uint64_t now = uptime_ms();
    const bool eof = input_.empty() && platform_->input_closed();
    for (auto& [pid, p] : procs_) {
        ProcessInfo& info = p->info_;
        switch (info.state) {
            case ProcState::Sleeping:
                if (now >= info.wake_at) info.state = ProcState::Ready;
                break;
            case ProcState::WaitInput:
                if (pid == foreground_ && (!input_.empty() || eof)) info.state = ProcState::Ready;
                break;
            case ProcState::WaitMessage:
                if (ipc_.pending(pid)) info.state = ProcState::Ready;
                break;
            case ProcState::WaitChild:
                if (!alive(info.waiting_for)) {
                    info.state = ProcState::Ready;
                    info.waiting_for = kNoPid;
                }
                break;
            default:
                break;
        }
    }
}

namespace {

// Process code runs inside Platform::run_protected, which takes a plain
// function pointer: these carry the call across.
struct QuantumCall {
    Kernel* kernel;
    Process* proc;
    ProcessInfo* info;
    uint64_t now;
};

struct ExitCall {
    Kernel* kernel;
    Process* proc;
};

} // namespace

void Kernel::run_quantum(Process& proc) {
    ProcessInfo& info = proc.info_;
    info.last_run = ++run_counter_;
    ++info.steps;

    QuantumCall call{this, &proc, &info, uptime_ms()};
    auto body = [](void* arg) {
        QuantumCall& q = *static_cast<QuantumCall*>(arg);
        Context ctx(*q.kernel, *q.proc);
        if (!q.info->started) {
            q.info->started = true;
            q.info->started_at = q.now;
            q.proc->on_start(ctx);
            if (q.info->state != ProcState::Ready) return;
        }
        q.proc->step(ctx);
    };

    String fault;
    if (!platform_->run_protected(body, &call, fault)) {
        // A faulting process must never take the kernel down with it.
        log(LogLevel::Error, format("pid %u (%s) crashed%s%s", info.pid, info.name.c_str(), fault.empty() ? "" : ": ",
                                    fault.c_str()));
        kill(info.pid, -1);
    }
}

void Kernel::reap() {
    Vector<Pid> dead;
    for (auto& [pid, p] : procs_)
        if (p->info_.state == ProcState::Zombie) dead.push_back(pid);

    for (Pid pid : dead) {
        auto it = procs_.find(pid);
        if (it == procs_.end()) continue;
        if (it->second->info_.started) {
            ExitCall call{this, it->second.get()};
            String ignored; // a crash in on_exit is not reported: the process is gone anyway
            platform_->run_protected(
                [](void* arg) {
                    ExitCall& e = *static_cast<ExitCall*>(arg);
                    Context ctx(*e.kernel, *e.proc);
                    e.proc->on_exit(ctx);
                },
                &call, ignored);
        }
        it = procs_.find(pid); // on_exit may have spawned processes
        UniquePtr<Process> proc = osef::move(it->second);
        procs_.erase(it);
        const ProcessInfo& info = proc->info_;

        ipc_.release(pid);
        for (auto& [opid, other] : procs_) {
            ProcessInfo& o = other->info_;
            if (o.parent == pid) o.parent = pid == init_pid_ ? kNoPid : init_pid_; // orphans go to init
            if (opid == info.parent) o.last_child_exit = info.exit_code;
            if (o.state == ProcState::WaitChild && o.waiting_for == pid) {
                o.state = ProcState::Ready;
                o.waiting_for = kNoPid;
                o.last_child_exit = info.exit_code;
            }
        }
        if (foreground_ == pid)
            foreground_ = alive(info.parent) ? info.parent : (alive(init_pid_) ? init_pid_ : kNoPid);

        log(LogLevel::Debug, format("exit pid %u (%s) code %d", pid, info.name.c_str(), info.exit_code));
        if (pid == init_pid_) {
            init_pid_ = kNoPid;
            if (running_) {
                log(LogLevel::Info, format("init exited with code %d", info.exit_code));
                shutdown(info.exit_code);
            }
        }
    }
}

void Kernel::halt() {
    if (halted_) return;
    running_ = false;
    // Terminate everything; on_exit handlers may spawn more, so bound the loop.
    for (int round = 0; round < 8 && !procs_.empty(); ++round) {
        for (auto& [pid, p] : procs_)
            if (p->info_.state != ProcState::Zombie) kill(pid, -1);
        reap();
    }
    for (size_t i = modules_.size(); i-- > 0;) modules_[i]->on_unload(*this);
    log(LogLevel::Info, "system halted");
    platform_->shutdown(exit_code_);
    halted_ = true;
}

} // namespace osef
