// Freestanding C++ runtime support: global operator new/delete on top of the
// kernel heap, the few Itanium-ABI hooks the compiler references, and the
// .init_array constructor runner. (Placement new is inline in osef core's
// base.h; memcpy & co. live in engine/src/core/cstring.cpp.)

#include "internal.h"

namespace std {
enum class align_val_t : size_t {};
struct nothrow_t {
    explicit nothrow_t() = default;
};
extern const nothrow_t nothrow;
} // namespace std

const std::nothrow_t std::nothrow{};

namespace {
inline void* alloc(size_t n) { return osef::bm::heap_alloc(n, 16); }
inline void* alloc(size_t n, std::align_val_t a) { return osef::bm::heap_alloc(n, static_cast<size_t>(a)); }
inline void release(void* p) { osef::bm::heap_free(p); }
} // namespace

// heap_alloc never returns null (it panics on OOM), so the nothrow variants
// behave like the throwing ones.
void* operator new(size_t n) { return alloc(n); }
void* operator new[](size_t n) { return alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return alloc(n); }
void* operator new(size_t n, std::align_val_t a) { return alloc(n, a); }
void* operator new[](size_t n, std::align_val_t a) { return alloc(n, a); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return alloc(n, a); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return alloc(n, a); }

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }

extern "C" {

void* __dso_handle = &__dso_handle;

// Static destructors never run: the kernel only ends by powering off.
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

[[noreturn]] void __cxa_pure_virtual() { osef::bm::panic("pure virtual function called"); }
[[noreturn]] void __cxa_deleted_virtual() { osef::bm::panic("deleted virtual function called"); }

using InitFn = void (*)();
extern InitFn __init_array_start[];
extern InitFn __init_array_end[];

} // extern "C"

namespace osef::bm {

void run_constructors() {
    for (InitFn* f = __init_array_start; f != __init_array_end; ++f) {
        if (*f && *f != reinterpret_cast<InitFn>(-1)) (*f)();
    }
}

} // namespace osef::bm
