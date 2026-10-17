#pragma once
// System call table. Your OS defines its own ABI by registering named calls.
// Every call goes through invoke(), so tracing and permission checks
// can be added in one place.

#include <osef/core.h>

namespace osef {

class Context;

using SyscallArgs = Vector<String>;

struct SyscallResult {
    int64_t code = 0;   // >= 0 success, < 0 error (errno-style)
    String data;
};

constexpr int64_t kENOSYS = -38;

using SyscallFn = Function<SyscallResult(Context&, const SyscallArgs&)>;

struct SyscallEntry {
    String description;
    SyscallFn fn;
};

class SyscallTable {
public:
    using Tracer = Function<void(Context&, StringView name, const SyscallArgs&, const SyscallResult&)>;

    void define(StringView name, String description, SyscallFn fn);
    bool exists(StringView name) const { return table_.contains(name); }
    const Map<String, SyscallEntry>& entries() const { return table_; }

    SyscallResult invoke(Context& ctx, StringView name, const SyscallArgs& args);
    void set_tracer(Tracer tracer) { tracer_ = osef::move(tracer); }

private:
    Map<String, SyscallEntry> table_;
    Tracer tracer_;
};

} // namespace osef
