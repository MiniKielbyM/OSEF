#pragma once
// OSEF core: owning smart pointers.
//
//   UniquePtr<T>  single owner, move-only, deletes with `delete`.
//   SharedPtr<T>  shared ownership with a non-atomic reference count (the
//                 engine is single-threaded). No weak pointers.

#include <osef/core/base.h>

namespace osef {

// ---------------------------------------------------------------------------
// UniquePtr
// ---------------------------------------------------------------------------
template <class T>
class UniquePtr {
    static_assert(!is_array_v<T>, "UniquePtr<T[]> is not supported; use Vector<T>");

public:
    using element_type = T;
    using pointer = T*;

    constexpr UniquePtr() noexcept = default;
    constexpr UniquePtr(nullptr_t) noexcept {}
    explicit UniquePtr(T* p) noexcept : ptr_(p) {}

    UniquePtr(UniquePtr&& other) noexcept : ptr_(other.release()) {}
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    UniquePtr(UniquePtr<U>&& other) noexcept : ptr_(other.release()) {}

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    ~UniquePtr() { reset(); }

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        reset(other.release()); // self-move safe: release() runs first
        return *this;
    }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    UniquePtr& operator=(UniquePtr<U>&& other) noexcept {
        reset(other.release());
        return *this;
    }
    UniquePtr& operator=(nullptr_t) noexcept {
        reset();
        return *this;
    }

    T* get() const noexcept { return ptr_; }
    T* release() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }
    // Takes ownership of p, then deletes the previous object (if different).
    void reset(T* p = nullptr) noexcept {
        T* old = ptr_;
        ptr_ = p;
        if (old && old != p) {
            static_assert(sizeof(T) > 0, "UniquePtr: cannot delete an incomplete type");
            delete old;
        }
    }
    void swap(UniquePtr& other) noexcept {
        T* p = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = p;
    }

    T& operator*() const {
        OSEF_ASSERT(ptr_ != nullptr);
        return *ptr_;
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

template <class T, class U>
bool operator==(const UniquePtr<T>& a, const UniquePtr<U>& b) noexcept { return a.get() == b.get(); }
template <class T, class U>
bool operator!=(const UniquePtr<T>& a, const UniquePtr<U>& b) noexcept { return a.get() != b.get(); }
template <class T>
bool operator==(const UniquePtr<T>& a, nullptr_t) noexcept { return a.get() == nullptr; }
template <class T>
bool operator==(nullptr_t, const UniquePtr<T>& a) noexcept { return a.get() == nullptr; }
template <class T>
bool operator!=(const UniquePtr<T>& a, nullptr_t) noexcept { return a.get() != nullptr; }
template <class T>
bool operator!=(nullptr_t, const UniquePtr<T>& a) noexcept { return a.get() != nullptr; }

template <class T, class... Args>
UniquePtr<T> make_unique(Args&&... args) {
    return UniquePtr<T>(new T(osef::forward<Args>(args)...));
}

// ---------------------------------------------------------------------------
// SharedPtr
// ---------------------------------------------------------------------------
namespace detail {

// Type-erased control block. `dispose` destroys the object and frees the
// block (there are no weak references, so both happen together).
struct SharedControl {
    long refs;
    void (*dispose)(SharedControl*) noexcept;
};

// Block for an object allocated separately (SharedPtr(new U)). Deletes it as
// U, so the right destructor runs even without a virtual destructor.
template <class U>
struct SharedPointerBlock {
    SharedControl control;
    U* object;
    static void dispose(SharedControl* c) noexcept {
        SharedPointerBlock* self = reinterpret_cast<SharedPointerBlock*>(c);
        delete self->object;
        delete self;
    }
};

// Block with the object stored inline (make_shared: one allocation).
template <class U>
struct SharedInlineBlock {
    SharedControl control;
    alignas(U) unsigned char storage[sizeof(U)];
    U* object() noexcept { return reinterpret_cast<U*>(storage); }
    static void dispose(SharedControl* c) noexcept {
        SharedInlineBlock* self = reinterpret_cast<SharedInlineBlock*>(c);
        self->object()->~U();
        delete self;
    }
};

} // namespace detail

template <class T>
class SharedPtr {
    static_assert(!is_array_v<T>, "SharedPtr<T[]> is not supported; use Vector<T>");

public:
    using element_type = T;

    constexpr SharedPtr() noexcept = default;
    constexpr SharedPtr(nullptr_t) noexcept {}

    // Takes ownership of p (deleted as U* when the last owner goes away).
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    explicit SharedPtr(U* p) {
        if (!p) return;
        static_assert(sizeof(U) > 0, "SharedPtr: cannot own an incomplete type");
        UniquePtr<U> guard(p); // deletes p if the control block allocation throws
        auto* block = new detail::SharedPointerBlock<U>;
        guard.release();
        block->control.refs = 1;
        block->control.dispose = &detail::SharedPointerBlock<U>::dispose;
        block->object = p;
        ptr_ = p;
        ctl_ = &block->control;
    }

    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    SharedPtr(UniquePtr<U>&& owner) : SharedPtr(owner.get()) {
        owner.release();
    }

    SharedPtr(const SharedPtr& other) noexcept : ptr_(other.ptr_), ctl_(other.ctl_) { retain(); }
    SharedPtr(SharedPtr&& other) noexcept : ptr_(other.ptr_), ctl_(other.ctl_) {
        other.ptr_ = nullptr;
        other.ctl_ = nullptr;
    }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    SharedPtr(const SharedPtr<U>& other) noexcept : ptr_(other.ptr_), ctl_(other.ctl_) {
        retain();
    }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    SharedPtr(SharedPtr<U>&& other) noexcept : ptr_(other.ptr_), ctl_(other.ctl_) {
        other.ptr_ = nullptr;
        other.ctl_ = nullptr;
    }

    // Aliasing: shares ownership with `owner` but points at `p`.
    template <class U>
    SharedPtr(const SharedPtr<U>& owner, T* p) noexcept : ptr_(p), ctl_(owner.ctl_) {
        retain();
    }

    ~SharedPtr() { drop(); }

    // Copy/move-and-swap: self-assignment safe, and the old object is
    // released only after the new one is held.
    SharedPtr& operator=(const SharedPtr& other) noexcept {
        SharedPtr(other).swap(*this);
        return *this;
    }
    SharedPtr& operator=(SharedPtr&& other) noexcept {
        SharedPtr(osef::move(other)).swap(*this);
        return *this;
    }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    SharedPtr& operator=(const SharedPtr<U>& other) noexcept {
        SharedPtr(other).swap(*this);
        return *this;
    }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    SharedPtr& operator=(SharedPtr<U>&& other) noexcept {
        SharedPtr(osef::move(other)).swap(*this);
        return *this;
    }
    SharedPtr& operator=(nullptr_t) noexcept {
        reset();
        return *this;
    }

    void reset() noexcept { SharedPtr().swap(*this); }
    template <class U, enable_if_t<is_convertible_v<U*, T*>, int> = 0>
    void reset(U* p) {
        SharedPtr(p).swap(*this);
    }

    void swap(SharedPtr& other) noexcept {
        T* p = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = p;
        detail::SharedControl* c = ctl_;
        ctl_ = other.ctl_;
        other.ctl_ = c;
    }

    T* get() const noexcept { return ptr_; }
    T& operator*() const {
        OSEF_ASSERT(ptr_ != nullptr);
        return *ptr_;
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    long use_count() const noexcept { return ctl_ ? ctl_->refs : 0; }
    bool unique() const noexcept { return use_count() == 1; }

private:
    template <class U> friend class SharedPtr;
    template <class U, class... Args> friend SharedPtr<U> make_shared(Args&&... args);

    SharedPtr(T* p, detail::SharedControl* c) noexcept : ptr_(p), ctl_(c) {}

    void retain() noexcept {
        if (ctl_) ++ctl_->refs;
    }
    void drop() noexcept {
        detail::SharedControl* c = ctl_;
        ptr_ = nullptr;
        ctl_ = nullptr;
        if (c && --c->refs == 0) c->dispose(c);
    }

    T* ptr_ = nullptr;
    detail::SharedControl* ctl_ = nullptr;
};

template <class T, class U>
bool operator==(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept { return a.get() == b.get(); }
template <class T, class U>
bool operator!=(const SharedPtr<T>& a, const SharedPtr<U>& b) noexcept { return a.get() != b.get(); }
template <class T>
bool operator==(const SharedPtr<T>& a, nullptr_t) noexcept { return a.get() == nullptr; }
template <class T>
bool operator==(nullptr_t, const SharedPtr<T>& a) noexcept { return a.get() == nullptr; }
template <class T>
bool operator!=(const SharedPtr<T>& a, nullptr_t) noexcept { return a.get() != nullptr; }
template <class T>
bool operator!=(nullptr_t, const SharedPtr<T>& a) noexcept { return a.get() != nullptr; }

// One allocation holding both the reference count and the object.
template <class T, class... Args>
SharedPtr<T> make_shared(Args&&... args) {
    using Block = detail::SharedInlineBlock<T>;
    // Frees the block if T's constructor throws (hosted builds).
    struct Guard {
        Block* block;
        ~Guard() { delete block; }
    } guard{new Block};
    T* obj = ::new (static_cast<void*>(guard.block->storage)) T(osef::forward<Args>(args)...);
    Block* block = guard.block;
    guard.block = nullptr;
    block->control.refs = 1;
    block->control.dispose = &Block::dispose;
    return SharedPtr<T>(obj, &block->control);
}

// Shares ownership, converting the pointer with static_cast.
template <class T, class U>
SharedPtr<T> static_pointer_cast(const SharedPtr<U>& p) noexcept {
    return SharedPtr<T>(p, static_cast<T*>(p.get()));
}

} // namespace osef
