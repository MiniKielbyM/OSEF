#pragma once
// OSEF core: Deque<T>, a double-ended queue in a growable ring buffer.
//
// push/pop at both ends are amortized O(1); operator[] is O(1). Growth
// invalidates references; pops invalidate only the removed element.

#include <osef/core/base.h>

namespace osef {

template <class T>
class Deque {
    template <bool Const>
    class Iter {
    public:
        using Owner = conditional_t<Const, const Deque, Deque>;
        using Ref = conditional_t<Const, const T&, T&>;
        using Ptr = conditional_t<Const, const T*, T*>;

        Iter() noexcept = default;
        Iter(Owner* d, size_t i) noexcept : d_(d), i_(i) {}
        // iterator -> const_iterator
        template <bool C = Const, enable_if_t<C, int> = 0>
        Iter(const Iter<false>& o) noexcept : d_(o.owner()), i_(o.index()) {}

        Ref operator*() const { return (*d_)[i_]; }
        Ptr operator->() const { return &(*d_)[i_]; }
        Iter& operator++() noexcept {
            ++i_;
            return *this;
        }
        Iter operator++(int) noexcept {
            Iter t = *this;
            ++i_;
            return t;
        }
        Iter& operator--() noexcept {
            --i_;
            return *this;
        }
        Iter operator--(int) noexcept {
            Iter t = *this;
            --i_;
            return t;
        }
        size_t index() const noexcept { return i_; }
        Owner* owner() const noexcept { return d_; }
        friend bool operator==(const Iter& a, const Iter& b) noexcept { return a.d_ == b.d_ && a.i_ == b.i_; }
        friend bool operator!=(const Iter& a, const Iter& b) noexcept { return !(a == b); }

    private:
        Owner* d_ = nullptr;
        size_t i_ = 0;
    };

public:
    using value_type = T;
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    Deque() noexcept = default;
    // Delegating to Deque() makes the destructor run if an element
    // constructor throws (hosted builds with exceptions).
    Deque(std::initializer_list<T> list) : Deque() {
        reserve(list.size());
        for (const T& v : list) push_back(v);
    }
    Deque(const Deque& other) : Deque() {
        reserve(other.size_);
        for (size_t i = 0; i < other.size_; ++i) push_back(other[i]);
    }
    Deque(Deque&& other) noexcept
        : buf_(other.buf_), cap_(other.cap_), head_(other.head_), size_(other.size_) {
        other.buf_ = nullptr;
        other.cap_ = other.head_ = other.size_ = 0;
    }
    ~Deque() {
        clear();
        detail::deallocate_bytes(buf_);
    }
    Deque& operator=(const Deque& other) {
        if (this != &other) Deque(other).swap(*this);
        return *this;
    }
    Deque& operator=(Deque&& other) noexcept {
        if (this != &other) Deque(osef::move(other)).swap(*this);
        return *this;
    }

    void swap(Deque& other) noexcept {
        T* b = buf_;
        buf_ = other.buf_;
        other.buf_ = b;
        size_t t = cap_;
        cap_ = other.cap_;
        other.cap_ = t;
        t = head_;
        head_ = other.head_;
        other.head_ = t;
        t = size_;
        size_ = other.size_;
        other.size_ = t;
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    size_t capacity() const noexcept { return cap_; }

    T& operator[](size_t i) {
        OSEF_ASSERT(i < size_);
        return buf_[slot(i)];
    }
    const T& operator[](size_t i) const {
        OSEF_ASSERT(i < size_);
        return buf_[slot(i)];
    }
    T& front() {
        OSEF_ASSERT(size_ > 0);
        return buf_[head_];
    }
    const T& front() const {
        OSEF_ASSERT(size_ > 0);
        return buf_[head_];
    }
    T& back() {
        OSEF_ASSERT(size_ > 0);
        return buf_[slot(size_ - 1)];
    }
    const T& back() const {
        OSEF_ASSERT(size_ > 0);
        return buf_[slot(size_ - 1)];
    }

    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, size_); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, size_); }

    void reserve(size_t n) {
        if (n > cap_) regrow(n, 0);
    }

    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(osef::move(v)); }
    void push_front(const T& v) { emplace_front(v); }
    void push_front(T&& v) { emplace_front(osef::move(v)); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == cap_) {
            // Construct first: args may refer to one of our elements.
            const size_t new_cap = next_capacity(size_ + 1);
            detail::StorageGuard<T> fresh(detail::allocate_array<T>(new_cap));
            ::new (static_cast<void*>(fresh.ptr + size_)) T(osef::forward<Args>(args)...);
            adopt(fresh.release(), new_cap, 0);
        } else {
            ::new (static_cast<void*>(buf_ + slot(size_))) T(osef::forward<Args>(args)...);
        }
        ++size_;
        return back();
    }

    template <class... Args>
    T& emplace_front(Args&&... args) {
        if (size_ == cap_) {
            const size_t new_cap = next_capacity(size_ + 1);
            detail::StorageGuard<T> fresh(detail::allocate_array<T>(new_cap));
            ::new (static_cast<void*>(fresh.ptr)) T(osef::forward<Args>(args)...);
            adopt(fresh.release(), new_cap, 1);
            head_ = 0;
        } else {
            const size_t h = (head_ + cap_ - 1) & (cap_ - 1);
            ::new (static_cast<void*>(buf_ + h)) T(osef::forward<Args>(args)...);
            head_ = h;
        }
        ++size_;
        return front();
    }

    void pop_front() {
        OSEF_ASSERT(size_ > 0);
        buf_[head_].~T();
        head_ = (head_ + 1) & (cap_ - 1);
        --size_;
    }
    void pop_back() {
        OSEF_ASSERT(size_ > 0);
        buf_[slot(size_ - 1)].~T();
        --size_;
    }

    // Removes the element at index i, preserving order.
    void erase_at(size_t i) {
        OSEF_ASSERT(i < size_);
        if (i < size_ / 2) {
            for (size_t j = i; j > 0; --j) (*this)[j] = osef::move((*this)[j - 1]);
            pop_front();
        } else {
            for (size_t j = i; j + 1 < size_; ++j) (*this)[j] = osef::move((*this)[j + 1]);
            pop_back();
        }
    }
    // Removes the element at pos; returns an iterator to the element after it.
    iterator erase(const_iterator pos) {
        OSEF_ASSERT(pos.owner() == this);
        const size_t i = pos.index();
        erase_at(i);
        return iterator(this, i);
    }

    void clear() noexcept {
        if constexpr (!is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < size_; ++i) buf_[slot(i)].~T();
        }
        size_ = 0;
        head_ = 0;
    }

private:
    size_t slot(size_t i) const noexcept { return (head_ + i) & (cap_ - 1); }

    // Capacity is always a power of two (or zero).
    size_t next_capacity(size_t needed) const {
        OSEF_ASSERT(needed <= max_elems());
        size_t c = cap_ ? cap_ : 8;
        while (c < needed) c *= 2;
        if (c == cap_) c *= 2;
        return c;
    }
    static constexpr size_t max_elems() noexcept {
        size_t m = 1;
        while (m <= SIZE_MAX / sizeof(T) / 4) m *= 2;
        return m;
    }

    // Moves the elements, in order, into `fresh` starting at offset.
    void adopt(T* fresh, size_t new_cap, size_t offset) noexcept {
        if constexpr (is_trivially_copyable_v<T>) {
            const size_t first = size_ < cap_ - head_ ? size_ : cap_ - head_;
            detail::relocate(fresh + offset, buf_ + head_, first);
            detail::relocate(fresh + offset + first, buf_, size_ - first);
        } else {
            for (size_t i = 0; i < size_; ++i) detail::relocate(fresh + offset + i, buf_ + slot(i), 1);
        }
        detail::deallocate_bytes(buf_);
        buf_ = fresh;
        cap_ = new_cap;
        head_ = offset;
    }
    void regrow(size_t n, size_t offset) {
        size_t c = 8;
        OSEF_ASSERT(n <= max_elems());
        while (c < n) c *= 2;
        adopt(detail::allocate_array<T>(c), c, offset);
    }

    T* buf_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;
    size_t size_ = 0;
};

} // namespace osef
