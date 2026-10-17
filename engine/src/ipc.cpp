#include <osef/ipc.h>
#include <osef/syscall.h>

namespace osef {

// ---------------------------------------------------------------- IPC

bool IPC::take(Pid self, Message& out) {
    auto it = boxes_.find(self);
    if (it == boxes_.end() || it->second.empty()) return false;
    out = osef::move(it->second.front());
    it->second.pop_front();
    return true;
}

size_t IPC::pending(Pid pid) const {
    auto it = boxes_.find(pid);
    return it == boxes_.end() ? 0 : it->second.size();
}

bool IPC::register_service(StringView name, Pid pid) { return services_.emplace(name, pid); }

Pid IPC::lookup(StringView name) const {
    auto it = services_.find(name);
    return it == services_.end() ? kNoPid : it->second;
}

void IPC::release(Pid pid) {
    boxes_.erase(pid);
    for (auto it = services_.begin(); it != services_.end();)
        it = it->second == pid ? services_.erase(it) : it + 1;
}

// ---------------------------------------------------------------- Syscalls

void SyscallTable::define(StringView name, String description, SyscallFn fn) {
    table_.insert_or_assign(String(name), SyscallEntry{osef::move(description), osef::move(fn)});
}

SyscallResult SyscallTable::invoke(Context& ctx, StringView name, const SyscallArgs& args) {
    SyscallResult result;
    auto it = table_.find(name);
    if (it == table_.end()) {
        result.code = kENOSYS;
        result.data = "no such syscall: " + name;
    } else {
        result = it->second.fn(ctx, args);
    }
    if (tracer_) tracer_(ctx, name, args, result);
    return result;
}

} // namespace osef
