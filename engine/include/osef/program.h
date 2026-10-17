#pragma once
// Program registry: the OS's "executables". Each program is a named factory
// that creates a fresh Process when it is run.

#include <osef/process.h>

namespace osef {

using ProcessFactory = Function<UniquePtr<Process>()>;

struct Program {
    String name;
    String description;
    ProcessFactory factory;
};

class ProgramRegistry {
public:
    void add(StringView name, String description, ProcessFactory factory) {
        programs_.insert_or_assign(String(name), Program{String(name), osef::move(description), osef::move(factory)});
    }

    template <class T>
    void add(StringView name, String description) {
        add(name, osef::move(description), []() -> UniquePtr<Process> { return make_unique<T>(); });
    }

    // A program whose step() is a lambda. Remember to call ctx.exit().
    void add_fn(StringView name, String description, FunctionProcess::Fn fn) {
        add(name, osef::move(description),
            [fn]() -> UniquePtr<Process> { return make_unique<FunctionProcess>(fn); });
    }

    const Program* find(StringView name) const {
        auto it = programs_.find(name);
        return it == programs_.end() ? nullptr : &it->second;
    }
    bool remove(StringView name) { return programs_.erase(name) != 0; }
    const Map<String, Program>& all() const { return programs_; }

private:
    Map<String, Program> programs_;
};

} // namespace osef
