#pragma once
// Modules are engine plugins, the OS equivalent of a game engine's systems.
// A module can register programs, syscalls, devices and file systems in
// on_load, run kernel-level logic every tick, and clean up in on_unload.

#include <osef/core.h>

namespace osef {

class Kernel;
struct KernelConfig;

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
    // Called when the module is added to a kernel that has not booted yet (and
    // for auto modules, from the Kernel constructor): adjust settings such as
    // the OS name, appearance or init program before anything is printed.
    virtual void on_configure(KernelConfig&) {}
    virtual void on_load(Kernel&) {}
    virtual void on_tick(Kernel&) {}
    virtual void on_unload(Kernel&) {}

    // The concrete type this module was added as (Kernel::add_module<T>).
    TypeId type() const noexcept { return type_; }

private:
    friend class Kernel;
    TypeId type_ = nullptr;
};

// ---------------------------------------------------------------- auto modules
// Modules that add themselves to every Kernel, without a line in main(). The
// OSEF editor uses this for the code it generates from its settings forms:
//
//   OSEF_AUTO_MODULE(MySettingsModule);
//
// Auto modules are added (in registration order) when a Kernel is constructed,
// before any module that main() adds.
struct AutoModule {
    const char* name;
    UniquePtr<Module> (*create)();
    TypeId type;
    AutoModule* next = nullptr;
};
void register_auto_module(AutoModule& module);
const AutoModule* auto_modules();

} // namespace osef

#define OSEF_DETAIL_CAT2(a, b) a##b
#define OSEF_DETAIL_CAT(a, b) OSEF_DETAIL_CAT2(a, b)
#define OSEF_AUTO_MODULE(Type)                                                                   \
    static ::osef::AutoModule OSEF_DETAIL_CAT(osef_auto_module_, __LINE__){                      \
        #Type, []() -> ::osef::UniquePtr<::osef::Module> { return ::osef::make_unique<Type>(); }, \
        ::osef::type_id<Type>()};                                                                \
    [[maybe_unused]] static const bool OSEF_DETAIL_CAT(osef_auto_module_registered_, __LINE__) = \
        (::osef::register_auto_module(OSEF_DETAIL_CAT(osef_auto_module_, __LINE__)), true)
