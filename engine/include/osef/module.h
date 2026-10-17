#pragma once
// Modules are engine plugins, the OS equivalent of a game engine's systems.
// A module can register programs, syscalls, devices and file systems in
// on_load, run kernel-level logic every tick, and clean up in on_unload.

#include <osef/core.h>

namespace osef {

class Kernel;

// Type tags: a unique address per type, so the kernel can find modules by
// type without RTTI (bare metal is built with -fno-rtti).
using TypeId = const void*;
namespace detail {
// Deliberately not const: mutable objects are never merged by the compiler
// or linker (identical constants may be), so every T gets its own address.
template <class T>
inline char type_tag = 0;
} // namespace detail
template <class T>
constexpr TypeId type_id() noexcept {
    return &detail::type_tag<remove_cv_t<T>>;
}

class Module {
public:
    virtual ~Module() = default;
    virtual const char* name() const = 0;
    virtual void on_load(Kernel&) {}
    virtual void on_tick(Kernel&) {}
    virtual void on_unload(Kernel&) {}

    // The concrete type this module was added as (Kernel::add_module<T>).
    TypeId type() const noexcept { return type_; }

private:
    friend class Kernel;
    TypeId type_ = nullptr;
};

} // namespace osef
