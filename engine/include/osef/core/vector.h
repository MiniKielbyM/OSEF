#pragma once
// OSEF core: Vector<T>, a contiguous growable array (like std::vector).
//
// Iterators are plain pointers. Any operation that grows the vector may
// reallocate and invalidates all iterators/references; insert and erase
// invalidate iterators at or after the position. Arguments that refer to an
// element of the same vector (v.push_back(v[0]), v.insert(p, v.back()), ...)
// are handled correctly.

#include <osef/core/base.h>

namespace osef {

template <class T>
class Vector {
public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    Vector() noexcept = default;

    // n value-initialized elements.
    // Constructors delegate to Vector() so the destructor cleans up if an
    // element constructor throws (hosted builds with exceptions).
    explicit Vector(size_t n) : Vector() {
        reserve(n);
        for (; size_ < n; ++size_) ::new (static_cast<void*>(data_ + size_)) T();
    }
    Vector(size_t n, const T& value) : Vector() {
        reserve(n);
        for (; size_ < n; ++size_) ::new (static_cast<void*>(data_ + size_)) T(value);
    }
    Vector(std::initializer_list<T> list) : Vector() {
        reserve(list.size());
        for (const T& v : list) {
            ::new (static_cast<void*>(data_ + size_)) T(v);
            ++size_;
        }
    }
    template <class It, enable_if_t<!is_integral_v<It>, int> = 0>
    Vector(It first, It last) : Vector() {
        if constexpr (is_pointer_v<It>) reserve(static_cast<size_t>(last - first));
        for (; first != last; ++first) emplace_back(*first);
    }

    Vector(const Vector& other) : Vector() {
        reserve(other.size_);
        for (; size_ < other.size_; ++size_) ::new (static_cast<void*>(data_ + size_)) T(other.data_[size_]);
    }
    Vector(Vector&& other) noexcept : data_(other.data_), size_(other.size_), cap_(other.cap_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_ = 0;
    }

    ~Vector() {
        detail::destroy_range(data_, size_);
        detail::deallocate_bytes(data_);
    }

    // Copy/move into a temporary first: self-assignment safe, and safe when
    // `other` is owned by one of our own elements.
    Vector& operator=(const Vector& other) {
        if (this != &other) Vector(other).swap(*this);
        return *this;
    }
    Vector& operator=(Vector&& other) noexcept {
        if (this != &other) Vector(osef::move(other)).swap(*this);
        return *this;
    }
    Vector& operator=(std::initializer_list<T> list) {
        Vector(list).swap(*this);
        return *this;
    }

    void swap(Vector& other) noexcept {
        T* d = data_;
        data_ = other.data_;
        other.data_ = d;
        size_t s = size_;
        size_ = other.size_;
        other.size_ = s;
        size_t c = cap_;
        cap_ = other.cap_;
        other.cap_ = c;
    }

    // --- access -----------------------------------------------------------
    T& operator[](size_t i) {
        OSEF_ASSERT(i < size_);
        return data_[i];
    }
    const T& operator[](size_t i) const {
        OSEF_ASSERT(i < size_);
        return data_[i];
    }
    T& front() {
        OSEF_ASSERT(size_ > 0);
        return data_[0];
    }
    const T& front() const {
        OSEF_ASSERT(size_ > 0);
        return data_[0];
    }
    T& back() {
        OSEF_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }
    const T& back() const {
        OSEF_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }
    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + size_; }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }
    static constexpr size_t max_size() noexcept { return SIZE_MAX / sizeof(T); }

    // --- capacity ---------------------------------------------------------
    void reserve(size_t n) {
        if (n > cap_) reallocate(n);
    }
    void shrink_to_fit() {
        if (cap_ > size_) reallocate(size_);
    }

    void resize(size_t n) {
        if (n <= size_) {
            truncate(n);
            return;
        }
        reserve(n);
        for (; size_ < n; ++size_) ::new (static_cast<void*>(data_ + size_)) T();
    }
    void resize(size_t n, const T& value) {
        if (n <= size_) {
            truncate(n);
            return;
        }
        if (n <= cap_) {
            for (; size_ < n; ++size_) ::new (static_cast<void*>(data_ + size_)) T(value);
            return;
        }
        // Build the new elements while `value` (which may be one of ours) is
        // still alive, then move the old elements over.
        const size_t new_cap = detail::grow_capacity(cap_, n, max_size());
        detail::StorageGuard<T> fresh(detail::allocate_array<T>(new_cap));
        size_t built = size_;
        struct Cleanup {
            T* base;
            size_t from;
            size_t* to;
            ~Cleanup() {
                if (base) detail::destroy_range(base + from, *to - from);
            }
        } cleanup{fresh.ptr, size_, &built};
        for (; built < n; ++built) ::new (static_cast<void*>(fresh.ptr + built)) T(value);
        cleanup.base = nullptr;
        detail::relocate(fresh.ptr, data_, size_);
        detail::deallocate_bytes(data_);
        data_ = fresh.release();
        cap_ = new_cap;
        size_ = n;
    }

    void clear() noexcept { truncate(0); }

    // --- modifiers --------------------------------------------------------
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(osef::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ < cap_) {
            ::new (static_cast<void*>(data_ + size_)) T(osef::forward<Args>(args)...);
        } else {
            // Construct in the new buffer before the old one goes away, so
            // args referring to our own elements stay valid.
            const size_t new_cap = detail::grow_capacity(cap_, size_ + 1, max_size());
            detail::StorageGuard<T> fresh(detail::allocate_array<T>(new_cap));
            ::new (static_cast<void*>(fresh.ptr + size_)) T(osef::forward<Args>(args)...);
            detail::relocate(fresh.ptr, data_, size_);
            detail::deallocate_bytes(data_);
            data_ = fresh.release();
            cap_ = new_cap;
        }
        return data_[size_++];
    }

    void pop_back() {
        OSEF_ASSERT(size_ > 0);
        --size_;
        data_[size_].~T();
    }

    // Inserts before pos; returns an iterator to the new element.
    T* insert(const T* pos, const T& value) { return emplace(pos, value); }
    T* insert(const T* pos, T&& value) { return emplace(pos, osef::move(value)); }

    template <class... Args>
    T* emplace(const T* pos, Args&&... args) {
        const size_t idx = index_of(pos);
        if (idx == size_) {
            emplace_back(osef::forward<Args>(args)...);
            return data_ + idx;
        }
        if (size_ < cap_) {
            T tmp(osef::forward<Args>(args)...); // before shifting: args may alias
            ::new (static_cast<void*>(data_ + size_)) T(osef::move(data_[size_ - 1]));
            for (size_t i = size_ - 1; i > idx; --i) data_[i] = osef::move(data_[i - 1]);
            data_[idx] = osef::move(tmp);
        } else {
            const size_t new_cap = detail::grow_capacity(cap_, size_ + 1, max_size());
            detail::StorageGuard<T> fresh(detail::allocate_array<T>(new_cap));
            ::new (static_cast<void*>(fresh.ptr + idx)) T(osef::forward<Args>(args)...);
            detail::relocate(fresh.ptr, data_, idx);
            detail::relocate(fresh.ptr + idx + 1, data_ + idx, size_ - idx);
            detail::deallocate_bytes(data_);
            data_ = fresh.release();
            cap_ = new_cap;
        }
        ++size_;
        return data_ + idx;
    }

    // Inserts [first, last) before pos (the range may come from this vector).
    template <class It, enable_if_t<!is_integral_v<It>, int> = 0>
    T* insert(const T* pos, It first, It last) {
        const size_t idx = index_of(pos);
        Vector items(first, last);
        const size_t k = items.size_;
        if (k == 0) return data_ + idx;
        OSEF_ASSERT(k <= max_size() - size_);
        if (idx == size_ && size_ + k <= cap_) {
            detail::relocate(data_ + size_, items.data_, k);
            items.size_ = 0;
            size_ += k;
            return data_ + idx;
        }
        const size_t new_cap =
            size_ + k <= cap_ ? cap_ : detail::grow_capacity(cap_, size_ + k, max_size());
        T* fresh = detail::allocate_array<T>(new_cap);
        detail::relocate(fresh, data_, idx);
        detail::relocate(fresh + idx, items.data_, k);
        items.size_ = 0;
        detail::relocate(fresh + idx + k, data_ + idx, size_ - idx);
        detail::deallocate_bytes(data_);
        data_ = fresh;
        cap_ = new_cap;
        size_ += k;
        return data_ + idx;
    }
    T* insert(const T* pos, std::initializer_list<T> list) { return insert(pos, list.begin(), list.end()); }
    // Inserts count copies of value before pos (value may be an element).
    T* insert(const T* pos, size_t count, const T& value) {
        const Vector items(count, value);
        return insert(pos, items.begin(), items.end());
    }

    // Removes the element at pos; returns an iterator to the element after it.
    T* erase(const T* pos) {
        const size_t idx = index_of(pos);
        OSEF_ASSERT(idx < size_);
        for (size_t i = idx; i + 1 < size_; ++i) data_[i] = osef::move(data_[i + 1]);
        pop_back();
        return data_ + idx;
    }
    // Removes [first, last); returns an iterator to the element after them.
    T* erase(const T* first, const T* last) {
        const size_t a = index_of(first);
        const size_t b = index_of(last);
        OSEF_ASSERT(a <= b);
        if (a == b) return data_ + a;
        const size_t k = b - a;
        for (size_t i = a; i + k < size_; ++i) data_[i] = osef::move(data_[i + k]);
        truncate(size_ - k);
        return data_ + a;
    }

private:
    size_t index_of(const T* pos) const {
        const size_t idx = static_cast<size_t>(pos - static_cast<const T*>(data_));
        OSEF_ASSERT(idx <= size_);
        return idx;
    }
    void truncate(size_t n) noexcept {
        detail::destroy_range(data_ + n, size_ - n);
        size_ = n;
    }
    void reallocate(size_t new_cap) {
        OSEF_ASSERT(new_cap >= size_ && new_cap <= max_size());
        T* fresh = new_cap ? detail::allocate_array<T>(new_cap) : nullptr;
        detail::relocate(fresh, data_, size_);
        detail::deallocate_bytes(data_);
        data_ = fresh;
        cap_ = new_cap;
    }

    T* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};

template <class T>
bool operator==(const Vector<T>& a, const Vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a.data()[i] == b.data()[i])) return false;
    return true;
}
template <class T>
bool operator!=(const Vector<T>& a, const Vector<T>& b) {
    return !(a == b);
}

} // namespace osef
