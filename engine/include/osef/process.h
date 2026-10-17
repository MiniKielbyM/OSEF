#pragma once
// Processes are the "game objects" of an OSEF operating system.
//
// OSEF uses cooperative, step-based multitasking: the kernel calls step()
// once per scheduling quantum, and the process does a small amount of work
// and returns. Blocking calls (read_line, receive, sleep, wait) don't block
// the CPU. They mark the process as waiting and return, and the kernel calls
// step() again once the condition is met. This keeps the engine 100%
// portable: no threads and no context switching, hosted or on bare metal.

#include <osef/core.h>

namespace osef {

class Context;
class Kernel;

using Pid = uint32_t;
constexpr Pid kNoPid = 0;

enum class ProcState { Ready, Sleeping, WaitInput, WaitMessage, WaitChild, Zombie };
const char* to_string(ProcState state);

// Process control block. Owned and maintained by the kernel.
struct ProcessInfo {
    Pid pid = kNoPid;
    Pid parent = kNoPid;
    String name;
    Vector<String> args;
    String cwd = "/";
    int priority = 0;
    ProcState state = ProcState::Ready;
    uint64_t wake_at = 0;        // Sleeping: uptime at which to wake
    Pid waiting_for = kNoPid;    // WaitChild: child pid
    int exit_code = 0;
    int last_child_exit = 0;
    uint64_t steps = 0;          // quanta consumed
    uint64_t last_run = 0;       // scheduler bookkeeping (global run counter)
    uint64_t started_at = 0;
    bool started = false;
};

class Process {
public:
    virtual ~Process() = default;

    virtual void on_start(Context&) {}
    virtual void step(Context& ctx) = 0;
    virtual void on_exit(Context&) {}

    const ProcessInfo& info() const { return info_; }

private:
    friend class Kernel;
    friend class Context;
    ProcessInfo info_;
};

// Wraps a lambda as a process; the lambda is the step() function.
class FunctionProcess final : public Process {
public:
    using Fn = Function<void(Context&)>;
    explicit FunctionProcess(Fn fn) : fn_(osef::move(fn)) {}
    void step(Context& ctx) override { fn_(ctx); }

private:
    Fn fn_;
};

} // namespace osef
