#pragma once
// OSEF core: Function<R(Args...)>, a copyable type-erased callable (like
// std::function). Holds lambdas (with captures), function pointers and
// other function objects; the callable is stored on the heap. Calling an
// empty Function asserts.

#include <osef/core/base.h>

namespace osef {

namespace detail {
template <class Void, class F, class... Args>
struct invoke_result_impl {};
template <class F, class... Args>
struct invoke_result_impl<void_t<decltype(declval<F>()(declval<Args>()...))>, F, Args...> {
    using type = decltype(declval<F>()(declval<Args>()...));
};

template <class R, class F, class Args, class = void>
inline constexpr bool is_invocable_r_impl = false;
template <class... A> struct TypeList {};
template <class R, class F, class... Args>
inline constexpr bool is_invocable_r_impl<R, F, TypeList<Args...>,
                                          void_t<typename invoke_result_impl<void, F, Args...>::type>> =
    is_void_v<R> || is_convertible_v<typename invoke_result_impl<void, F, Args...>::type, R>;
} // namespace detail

// True when F can be called with Args and the result converts to R (any
// result is fine for R = void).
template <class R, class F, class... Args>
inline constexpr bool is_invocable_r_v = detail::is_invocable_r_impl<R, F, detail::TypeList<Args...>>;

template <class Signature>
class Function;

template <class R, class... Args>
class Function<R(Args...)> {
    struct VTable {
        R (*invoke)(void*, Args&&...);
        void* (*clone)(const void*);
        void (*destroy)(void*) noexcept;
    };

    template <class F>
    struct Ops {
        static R invoke(void* obj, Args&&... args) {
            if constexpr (is_void_v<R>) {
                (*static_cast<F*>(obj))(osef::forward<Args>(args)...);
            } else {
                return (*static_cast<F*>(obj))(osef::forward<Args>(args)...);
            }
        }
        static void* clone(const void* obj) { return new F(*static_cast<const F*>(obj)); }
        static void destroy(void* obj) noexcept { delete static_cast<F*>(obj); }
        static constexpr VTable table{&invoke, &clone, &destroy};
    };

public:
    using result_type = R;

    Function() noexcept = default;
    Function(nullptr_t) noexcept {}

    template <class F, enable_if_t<!is_same_v<decay_t<F>, Function> && !is_same_v<decay_t<F>, nullptr_t> &&
                                       is_invocable_r_v<R, decay_t<F>&, Args...>,
                                   int> = 0>
    Function(F&& f) {
        using Fn = decay_t<F>;
        if constexpr (is_pointer_v<remove_reference_t<F>>) {
            if (f == nullptr) return; // a null function pointer gives an empty Function
        }
        obj_ = new Fn(osef::forward<F>(f));
        vt_ = &Ops<Fn>::table;
    }

    Function(const Function& other) : obj_(other.vt_ ? other.vt_->clone(other.obj_) : nullptr), vt_(other.vt_) {}
    Function(Function&& other) noexcept : obj_(other.obj_), vt_(other.vt_) {
        other.obj_ = nullptr;
        other.vt_ = nullptr;
    }
    ~Function() {
        if (vt_) vt_->destroy(obj_);
    }

    Function& operator=(const Function& other) {
        if (this != &other) Function(other).swap(*this);
        return *this;
    }
    Function& operator=(Function&& other) noexcept {
        if (this != &other) Function(osef::move(other)).swap(*this);
        return *this;
    }
    Function& operator=(nullptr_t) noexcept {
        Function().swap(*this);
        return *this;
    }
    template <class F, enable_if_t<!is_same_v<decay_t<F>, Function> && !is_same_v<decay_t<F>, nullptr_t> &&
                                       is_invocable_r_v<R, decay_t<F>&, Args...>,
                                   int> = 0>
    Function& operator=(F&& f) {
        Function(osef::forward<F>(f)).swap(*this);
        return *this;
    }

    void swap(Function& other) noexcept {
        void* o = obj_;
        obj_ = other.obj_;
        other.obj_ = o;
        const VTable* v = vt_;
        vt_ = other.vt_;
        other.vt_ = v;
    }

    explicit operator bool() const noexcept { return vt_ != nullptr; }

    R operator()(Args... args) const {
        OSEF_ASSERT(vt_ != nullptr);
        return vt_->invoke(obj_, osef::forward<Args>(args)...);
    }

private:
    void* obj_ = nullptr;
    const VTable* vt_ = nullptr;
};

template <class R, class... Args>
bool operator==(const Function<R(Args...)>& f, nullptr_t) noexcept { return !f; }
template <class R, class... Args>
bool operator==(nullptr_t, const Function<R(Args...)>& f) noexcept { return !f; }
template <class R, class... Args>
bool operator!=(const Function<R(Args...)>& f, nullptr_t) noexcept { return static_cast<bool>(f); }
template <class R, class... Args>
bool operator!=(nullptr_t, const Function<R(Args...)>& f) noexcept { return static_cast<bool>(f); }

} // namespace osef
