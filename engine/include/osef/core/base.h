#pragma once
// OSEF core: configuration macros, language support and type utilities.
//
// The core library is freestanding: it only uses <stddef.h>, <stdint.h>,
// <stdarg.h>, <limits.h> and <float.h>, so the same engine code compiles as a
// hosted application and as a bare-metal kernel. In hosted builds it can be
// mixed freely with the C++ standard library in the same translation unit.
//
// Configuration:
//   OSEF_BAREMETAL   defined to 1 by the bare-metal build. Hosted builds leave
//                    it undefined (hardware.h tests `defined(OSEF_BAREMETAL)`,
//                    so the core never defines it). Test it with
//                    `#if OSEF_BAREMETAL` or use OSEF_HOSTED.
//   OSEF_HOSTED      always defined: 1 for hosted builds, 0 for bare metal.
//   OSEF_HAS_EXCEPTIONS  1 when hosted and compiled with exceptions enabled.

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#if defined(OSEF_BAREMETAL) && OSEF_BAREMETAL
#  define OSEF_HOSTED 0
#else
#  define OSEF_HOSTED 1
#endif

#if OSEF_HOSTED && (defined(__cpp_exceptions) || defined(_CPPUNWIND))
#  define OSEF_HAS_EXCEPTIONS 1
#else
#  define OSEF_HAS_EXCEPTIONS 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define OSEF_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#  define OSEF_PRINTF_FORMAT(fmt_index, first_arg)
#endif

// ---------------------------------------------------------------------------
// Placement new and std::initializer_list.
// Hosted builds take them from the standard library; bare metal has no C++
// library headers (-nostdinc++), so the core provides compatible definitions.
// ---------------------------------------------------------------------------
#if OSEF_HOSTED
#  include <initializer_list>
#  include <new>
#else
#  ifndef OSEF_PLACEMENT_NEW_DEFINED
#    define OSEF_PLACEMENT_NEW_DEFINED 1
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }
inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}
#  endif
#  ifndef OSEF_INITIALIZER_LIST_DEFINED
#    define OSEF_INITIALIZER_LIST_DEFINED 1
namespace std {
// Layout (pointer + length) matches what clang and gcc construct for braced
// lists; the private constructor is the one the compiler calls.
template <class E>
class initializer_list {
public:
    using value_type = E;
    using reference = const E&;
    using const_reference = const E&;
    using size_type = size_t;
    using iterator = const E*;
    using const_iterator = const E*;

    constexpr initializer_list() noexcept : begin_(nullptr), size_(0) {}
    constexpr size_t size() const noexcept { return size_; }
    constexpr const E* begin() const noexcept { return begin_; }
    constexpr const E* end() const noexcept { return begin_ + size_; }

private:
    constexpr initializer_list(const E* b, size_t n) noexcept : begin_(b), size_(n) {}
    const E* begin_;
    size_t size_;
};
template <class E>
constexpr const E* begin(initializer_list<E> il) noexcept { return il.begin(); }
template <class E>
constexpr const E* end(initializer_list<E> il) noexcept { return il.end(); }
} // namespace std
#  endif
#endif

#if !defined(__GNUC__) && !defined(__clang__)
// MSVC (hosted only): no __builtin_mem* family, use the C library.
#  include <string.h>
#endif

namespace osef {

using ::size_t;
using ::ptrdiff_t;
using nullptr_t = decltype(nullptr);

// ---------------------------------------------------------------------------
// Assertions. assert_fail is provided by the platform layer (hosted: print and
// abort; bare metal: kernel panic).
// ---------------------------------------------------------------------------
[[noreturn]] void assert_fail(const char* file, int line, const char* expr);

#define OSEF_ASSERT(cond) ((cond) ? static_cast<void>(0) : ::osef::assert_fail(__FILE__, __LINE__, #cond))

// ---------------------------------------------------------------------------
// Type traits (only what the engine needs; names follow <type_traits>).
// ---------------------------------------------------------------------------
template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T&> { using type = T; };
template <class T> struct remove_reference<T&&> { using type = T; };
template <class T> using remove_reference_t = typename remove_reference<T>::type;

template <class T> struct remove_cv { using type = T; };
template <class T> struct remove_cv<const T> { using type = T; };
template <class T> struct remove_cv<volatile T> { using type = T; };
template <class T> struct remove_cv<const volatile T> { using type = T; };
template <class T> using remove_cv_t = typename remove_cv<T>::type;

template <class T> using remove_cvref_t = remove_cv_t<remove_reference_t<T>>;

template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { using type = T; };
template <bool B, class T = void> using enable_if_t = typename enable_if<B, T>::type;

template <bool B, class T, class F> struct conditional { using type = T; };
template <class T, class F> struct conditional<false, T, F> { using type = F; };
template <bool B, class T, class F> using conditional_t = typename conditional<B, T, F>::type;

template <class...> using void_t = void;

template <class A, class B> inline constexpr bool is_same_v = false;
template <class A> inline constexpr bool is_same_v<A, A> = true;

template <class T> inline constexpr bool is_const_v = false;
template <class T> inline constexpr bool is_const_v<const T> = true;

template <class T> inline constexpr bool is_reference_v = false;
template <class T> inline constexpr bool is_reference_v<T&> = true;
template <class T> inline constexpr bool is_reference_v<T&&> = true;

template <class T> inline constexpr bool is_lvalue_reference_v = false;
template <class T> inline constexpr bool is_lvalue_reference_v<T&> = true;

template <class T> inline constexpr bool is_void_v = is_same_v<remove_cv_t<T>, void>;

namespace detail {
template <class T> inline constexpr bool is_pointer_impl = false;
template <class T> inline constexpr bool is_pointer_impl<T*> = true;
template <class T> inline constexpr bool is_integral_impl = false;
template <> inline constexpr bool is_integral_impl<bool> = true;
template <> inline constexpr bool is_integral_impl<char> = true;
template <> inline constexpr bool is_integral_impl<signed char> = true;
template <> inline constexpr bool is_integral_impl<unsigned char> = true;
template <> inline constexpr bool is_integral_impl<wchar_t> = true;
#if defined(__cpp_char8_t)
template <> inline constexpr bool is_integral_impl<char8_t> = true;
#endif
template <> inline constexpr bool is_integral_impl<char16_t> = true;
template <> inline constexpr bool is_integral_impl<char32_t> = true;
template <> inline constexpr bool is_integral_impl<short> = true;
template <> inline constexpr bool is_integral_impl<unsigned short> = true;
template <> inline constexpr bool is_integral_impl<int> = true;
template <> inline constexpr bool is_integral_impl<unsigned int> = true;
template <> inline constexpr bool is_integral_impl<long> = true;
template <> inline constexpr bool is_integral_impl<unsigned long> = true;
template <> inline constexpr bool is_integral_impl<long long> = true;
template <> inline constexpr bool is_integral_impl<unsigned long long> = true;
} // namespace detail

template <class T> inline constexpr bool is_pointer_v = detail::is_pointer_impl<remove_cv_t<T>>;
template <class T> inline constexpr bool is_integral_v = detail::is_integral_impl<remove_cv_t<T>>;

template <class T> inline constexpr bool is_array_v = false;
template <class T> inline constexpr bool is_array_v<T[]> = true;
template <class T, size_t N> inline constexpr bool is_array_v<T[N]> = true;

template <class T> struct remove_extent { using type = T; };
template <class T> struct remove_extent<T[]> { using type = T; };
template <class T, size_t N> struct remove_extent<T[N]> { using type = T; };
template <class T> using remove_extent_t = typename remove_extent<T>::type;

// Only function types (and references) cannot be const-qualified.
template <class T> inline constexpr bool is_function_v = !is_const_v<const T> && !is_reference_v<T>;

template <class T> struct decay {
private:
    using U = remove_reference_t<T>;

public:
    using type = conditional_t<is_array_v<U>, remove_extent_t<U>*,
                               conditional_t<is_function_v<U>, U*, remove_cv_t<U>>>;
};
template <class T> using decay_t = typename decay<T>::type;

template <class T> T&& declval() noexcept; // unevaluated contexts only

template <class Base, class Derived>
inline constexpr bool is_base_of_v = __is_base_of(Base, Derived);

template <class T>
inline constexpr bool is_trivially_copyable_v = __is_trivially_copyable(T);

#if defined(__clang__) || defined(_MSC_VER)
template <class T>
inline constexpr bool is_trivially_destructible_v = __is_trivially_destructible(T);
#else
#  if defined(__has_builtin)
#    if __has_builtin(__is_trivially_destructible)
#      define OSEF_DETAIL_TRIVIAL_DTOR(T) __is_trivially_destructible(T)
#    endif
#  endif
#  ifndef OSEF_DETAIL_TRIVIAL_DTOR
#    define OSEF_DETAIL_TRIVIAL_DTOR(T) __has_trivial_destructor(T)
#  endif
template <class T>
inline constexpr bool is_trivially_destructible_v = OSEF_DETAIL_TRIVIAL_DTOR(T);
#endif

namespace detail {
template <class To> void convert_to(To) noexcept;
template <class From, class To, class = void>
struct is_convertible_impl { static constexpr bool value = false; };
template <class From, class To>
struct is_convertible_impl<From, To, void_t<decltype(convert_to<To>(declval<From>()))>> {
    static constexpr bool value = true;
};
} // namespace detail

// Portable is_convertible: implicit conversion From -> To (void -> void true).
template <class From, class To>
inline constexpr bool is_convertible_v =
    (is_void_v<From> && is_void_v<To>) ||
    (!is_void_v<From> && !is_void_v<To> && !is_array_v<To> && !is_function_v<To> &&
     detail::is_convertible_impl<From, To>::value);

// ---------------------------------------------------------------------------
// Utilities. Always call these qualified (osef::move) inside templates, so
// argument-dependent lookup never mixes them up with std::move and friends.
// ---------------------------------------------------------------------------
template <class T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

template <class T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}
template <class T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    static_assert(!is_lvalue_reference_v<T>, "osef::forward: cannot forward an rvalue as an lvalue");
    return static_cast<T&&>(t);
}

// osef::swap(a, b) is a function object, not a function template: argument-
// dependent lookup ignores objects, so `using std::swap; swap(x, y);` (as in
// std::sort, std::iter_swap, ...) on osef types never sees two equally good
// swap(T&, T&) templates. It calls a.swap(b) when T has that member and
// otherwise swaps through a temporary. Do not add free swap() overloads to
// namespace osef (they would clash with this object); add a member swap.
namespace detail {
template <class T, class = void>
inline constexpr bool has_member_swap = false;
template <class T>
inline constexpr bool has_member_swap<T, void_t<decltype(declval<T&>().swap(declval<T&>()))>> = true;

struct SwapFn {
    template <class T>
    constexpr void operator()(T& a, T& b) const noexcept {
        if constexpr (has_member_swap<T>) {
            a.swap(b);
        } else {
            T tmp(osef::move(a));
            a = osef::move(b);
            b = osef::move(tmp);
        }
    }
};
} // namespace detail
inline constexpr detail::SwapFn swap{};

template <class T, class U = T>
constexpr T exchange(T& obj, U&& new_value) {
    T old(osef::move(obj));
    obj = osef::forward<U>(new_value);
    return old;
}

// Parenthesized names keep these usable when a platform header defines
// min/max macros.
template <class T>
constexpr const T&(min)(const T& a, const T& b) { return b < a ? b : a; }
template <class T>
constexpr const T&(max)(const T& a, const T& b) { return a < b ? b : a; }

// ---------------------------------------------------------------------------
// Raw memory helpers used by the containers.
// ---------------------------------------------------------------------------
namespace detail {

#if defined(__GNUC__) || defined(__clang__)
inline void* mem_copy(void* d, const void* s, size_t n) noexcept { return __builtin_memcpy(d, s, n); }
inline void* mem_move(void* d, const void* s, size_t n) noexcept { return __builtin_memmove(d, s, n); }
inline void* mem_set(void* d, int c, size_t n) noexcept { return __builtin_memset(d, c, n); }
inline int mem_compare(const void* a, const void* b, size_t n) noexcept { return __builtin_memcmp(a, b, n); }
constexpr size_t cstr_length(const char* s) noexcept { return __builtin_strlen(s); }
#else
inline void* mem_copy(void* d, const void* s, size_t n) noexcept { return ::memcpy(d, s, n); }
inline void* mem_move(void* d, const void* s, size_t n) noexcept { return ::memmove(d, s, n); }
inline void* mem_set(void* d, int c, size_t n) noexcept { return ::memset(d, c, n); }
inline int mem_compare(const void* a, const void* b, size_t n) noexcept { return ::memcmp(a, b, n); }
constexpr size_t cstr_length(const char* s) noexcept {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}
#endif

#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
inline constexpr size_t kNewAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
inline constexpr size_t kNewAlignment = alignof(::max_align_t);
#endif

inline void* allocate_bytes(size_t bytes) {
    void* p = ::operator new(bytes);
    OSEF_ASSERT(p != nullptr);
    return p;
}
inline void deallocate_bytes(void* p) noexcept {
    if (p) ::operator delete(p);
}

// Uninitialized storage for n objects of type T.
template <class T>
T* allocate_array(size_t n) {
    static_assert(alignof(T) <= kNewAlignment, "osef containers do not support over-aligned types");
    OSEF_ASSERT(n <= SIZE_MAX / sizeof(T));
    return static_cast<T*>(allocate_bytes(n * sizeof(T)));
}

// Frees raw storage on scope exit unless released (exception/early-exit safety).
template <class T>
struct StorageGuard {
    T* ptr;
    explicit StorageGuard(T* p) noexcept : ptr(p) {}
    StorageGuard(const StorageGuard&) = delete;
    StorageGuard& operator=(const StorageGuard&) = delete;
    ~StorageGuard() { deallocate_bytes(ptr); }
    T* release() noexcept {
        T* p = ptr;
        ptr = nullptr;
        return p;
    }
};

template <class T>
void destroy_range(T* first, size_t n) noexcept {
    if constexpr (!is_trivially_destructible_v<T>) {
        for (size_t i = 0; i < n; ++i) first[i].~T();
    } else {
        (void)first;
        (void)n;
    }
}

// Moves n objects from src into uninitialized dst and destroys the sources.
// The ranges must not overlap.
template <class T>
void relocate(T* dst, T* src, size_t n) noexcept {
    if (n == 0) return;
    if constexpr (is_trivially_copyable_v<T>) {
        mem_copy(static_cast<void*>(dst), static_cast<const void*>(src), n * sizeof(T));
    } else {
        for (size_t i = 0; i < n; ++i) {
            ::new (static_cast<void*>(dst + i)) T(osef::move(src[i]));
            src[i].~T();
        }
    }
}

// True when p points inside [first, first + n). Integer comparison avoids the
// unspecified ordering of unrelated pointers.
template <class T>
bool points_into(const T* p, const T* first, size_t n) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t b = reinterpret_cast<uintptr_t>(first);
    return a >= b && a < b + n * sizeof(T);
}

// Capacity growth: doubling (amortized O(1) appends), at least `needed`.
inline size_t grow_capacity(size_t current, size_t needed, size_t max_elems, size_t minimum = 4) {
    OSEF_ASSERT(needed <= max_elems);
    size_t cap = current > max_elems / 2 ? max_elems : current * 2;
    if (cap < minimum) cap = minimum;
    if (cap < needed) cap = needed;
    if (cap > max_elems) cap = max_elems;
    return cap;
}

} // namespace detail
} // namespace osef
