// OSEF core: String implementation and integer <-> text conversions.

#include <osef/core/string.h>

namespace osef {

// ---------------------------------------------------------------------------
// Storage management
// ---------------------------------------------------------------------------
void String::release_heap() noexcept {
    if (!is_local()) detail::deallocate_bytes(ptr_);
}

void String::take(String& other) noexcept {
    if (other.is_local()) {
        detail::mem_copy(local_, other.local_, other.size_ + 1);
        ptr_ = local_;
        cap_ = kLocalCap;
    } else {
        ptr_ = other.ptr_;
        cap_ = other.cap_;
        other.ptr_ = other.local_;
        other.cap_ = kLocalCap;
    }
    size_ = other.size_;
    other.size_ = 0;
    other.local_[0] = '\0';
}

size_t String::grown_capacity(size_t needed) const {
    // One byte is always reserved for the terminator.
    return detail::grow_capacity(cap_, needed, SIZE_MAX - 1, 0);
}

void String::reallocate(size_t new_cap) {
    OSEF_ASSERT(new_cap >= size_);
    char* fresh;
    if (new_cap <= kLocalCap) {
        if (is_local()) return;
        fresh = local_;
        new_cap = kLocalCap;
    } else {
        fresh = static_cast<char*>(detail::allocate_bytes(new_cap + 1));
    }
    detail::mem_copy(fresh, ptr_, size_ + 1);
    release_heap();
    ptr_ = fresh;
    cap_ = new_cap;
}

void String::reserve(size_t n) {
    OSEF_ASSERT(n < SIZE_MAX); // n + 1 bytes are allocated (terminator)
    if (n > cap_) reallocate(n);
}

void String::shrink_to_fit() {
    if (cap_ > size_ && !is_local()) reallocate(size_);
}

void String::swap(String& other) noexcept {
    if (this == &other) return;
    String tmp(osef::move(other));
    other.take(*this);
    // *this is now empty and local.
    take(tmp);
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------
String& String::assign(const char* s, size_t n) {
    if (n <= cap_) {
        if (n) detail::mem_move(ptr_, s, n); // s may alias our buffer
    } else {
        const size_t new_cap = grown_capacity(n);
        char* fresh = static_cast<char*>(detail::allocate_bytes(new_cap + 1));
        detail::mem_copy(fresh, s, n); // copy before freeing: s may alias
        release_heap();
        ptr_ = fresh;
        cap_ = new_cap;
    }
    size_ = n;
    ptr_[n] = '\0';
    return *this;
}

String& String::append(const char* s, size_t n) {
    if (n == 0) return *this;
    OSEF_ASSERT(n < SIZE_MAX - 1 - size_);
    const size_t total = size_ + n;
    if (total <= cap_) {
        detail::mem_move(ptr_ + size_, s, n);
    } else {
        const size_t new_cap = grown_capacity(total);
        char* fresh = static_cast<char*>(detail::allocate_bytes(new_cap + 1));
        detail::mem_copy(fresh, ptr_, size_);
        detail::mem_copy(fresh + size_, s, n); // old buffer still alive: s may alias
        release_heap();
        ptr_ = fresh;
        cap_ = new_cap;
    }
    size_ = total;
    ptr_[total] = '\0';
    return *this;
}

String& String::append(size_t n, char c) {
    if (n == 0) return *this;
    OSEF_ASSERT(n < SIZE_MAX - 1 - size_);
    const size_t total = size_ + n;
    if (total > cap_) reallocate(grown_capacity(total));
    detail::mem_set(ptr_ + size_, static_cast<unsigned char>(c), n);
    size_ = total;
    ptr_[total] = '\0';
    return *this;
}

void String::resize(size_t n, char c) {
    if (n <= size_) {
        size_ = n;
        ptr_[n] = '\0';
    } else {
        append(n - size_, c);
    }
}

String& String::replace(size_t pos, size_t n, StringView sv) {
    if (pos > size_) pos = size_;
    if (n > size_ - pos) n = size_ - pos;
    if (sv.size() != 0 && detail::points_into(sv.data(), static_cast<const char*>(ptr_), size_ + 1)) {
        const String copy(sv); // the source is part of this string
        return replace(pos, n, copy.view());
    }
    const size_t m = sv.size();
    OSEF_ASSERT(m - n < SIZE_MAX - 1 - size_ || m <= n);
    const size_t total = size_ - n + m;
    const size_t tail = size_ - pos - n;
    if (total > cap_) {
        const size_t new_cap = grown_capacity(total);
        char* fresh = static_cast<char*>(detail::allocate_bytes(new_cap + 1));
        detail::mem_copy(fresh, ptr_, pos);
        if (m) detail::mem_copy(fresh + pos, sv.data(), m);
        detail::mem_copy(fresh + pos + m, ptr_ + pos + n, tail);
        release_heap();
        ptr_ = fresh;
        cap_ = new_cap;
    } else {
        detail::mem_move(ptr_ + pos + m, ptr_ + pos + n, tail);
        if (m) detail::mem_copy(ptr_ + pos, sv.data(), m);
    }
    size_ = total;
    ptr_[total] = '\0';
    return *this;
}

String& String::insert(size_t pos, StringView sv) { return replace(pos, 0, sv); }

String& String::insert(size_t pos, size_t n, char c) {
    if (n == 0) return *this;
    if (pos > size_) pos = size_;
    OSEF_ASSERT(n < SIZE_MAX - 1 - size_);
    const size_t total = size_ + n;
    if (total > cap_) reallocate(grown_capacity(total));
    detail::mem_move(ptr_ + pos + n, ptr_ + pos, size_ - pos);
    detail::mem_set(ptr_ + pos, static_cast<unsigned char>(c), n);
    size_ = total;
    ptr_[total] = '\0';
    return *this;
}

String& String::erase(size_t pos, size_t n) {
    if (pos > size_) pos = size_;
    if (n > size_ - pos) n = size_ - pos;
    if (n == 0) return *this;
    detail::mem_move(ptr_ + pos, ptr_ + pos + n, size_ - pos - n);
    size_ -= n;
    ptr_[size_] = '\0';
    return *this;
}

// ---------------------------------------------------------------------------
// Concatenation
// ---------------------------------------------------------------------------
namespace {
String concat(StringView a, StringView b) {
    String s;
    s.reserve(a.size() + b.size());
    s.append(a);
    s.append(b);
    return s;
}
} // namespace

String operator+(const String& a, const String& b) { return concat(a, b); }
String operator+(const String& a, StringView b) { return concat(a, b); }
String operator+(StringView a, const String& b) { return concat(a, b); }
String operator+(const String& a, const char* b) { return concat(a, b); }
String operator+(const char* a, const String& b) { return concat(a, b); }
String operator+(const String& a, char b) { return concat(a, StringView(&b, 1)); }
String operator+(char a, const String& b) { return concat(StringView(&a, 1), b); }
String operator+(StringView a, StringView b) { return concat(a, b); }
String operator+(StringView a, const char* b) { return concat(a, b); }
String operator+(const char* a, StringView b) { return concat(a, b); }
String operator+(StringView a, char b) { return concat(a, StringView(&b, 1)); }
String operator+(char a, StringView b) { return concat(StringView(&a, 1), b); }
String operator+(String&& a, const String& b) { return osef::move(a.append(b)); }
String operator+(String&& a, StringView b) { return osef::move(a.append(b)); }
String operator+(String&& a, const char* b) { return osef::move(a.append(b)); }
String operator+(String&& a, char b) {
    a.push_back(b);
    return osef::move(a);
}

// ---------------------------------------------------------------------------
// Integer conversions
// ---------------------------------------------------------------------------
namespace {
String unsigned_to_string(unsigned long long v, bool negative) {
    char buf[24];
    size_t i = sizeof(buf);
    do {
        buf[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    if (negative) buf[--i] = '-';
    return String(buf + i, sizeof(buf) - i);
}
String signed_to_string(long long v) {
    // Negate in unsigned arithmetic: well defined for LLONG_MIN.
    const unsigned long long mag = v < 0 ? 0ull - static_cast<unsigned long long>(v)
                                         : static_cast<unsigned long long>(v);
    return unsigned_to_string(mag, v < 0);
}
int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}
} // namespace

String to_string(int v) { return signed_to_string(v); }
String to_string(unsigned v) { return unsigned_to_string(v, false); }
String to_string(long v) { return signed_to_string(v); }
String to_string(unsigned long v) { return unsigned_to_string(v, false); }
String to_string(long long v) { return signed_to_string(v); }
String to_string(unsigned long long v) { return unsigned_to_string(v, false); }

bool parse_int(StringView text, long long& out) {
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text.data()[i] == '+' || text.data()[i] == '-')) {
        negative = text.data()[i] == '-';
        ++i;
    }
    unsigned base = 10;
    if (i + 1 < text.size() && text.data()[i] == '0' && (text.data()[i + 1] == 'x' || text.data()[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i == text.size()) return false; // no digits
    const unsigned long long limit =
        negative ? static_cast<unsigned long long>(LLONG_MAX) + 1 : static_cast<unsigned long long>(LLONG_MAX);
    unsigned long long value = 0;
    for (; i < text.size(); ++i) {
        const int d = digit_value(text.data()[i]);
        if (d >= static_cast<int>(base)) return false;
        if (value > (limit - static_cast<unsigned>(d)) / base) return false; // overflow
        value = value * base + static_cast<unsigned>(d);
    }
    // Conversion of an out-of-range unsigned value is modular in C++20.
    out = negative ? static_cast<long long>(0ull - value) : static_cast<long long>(value);
    return true;
}

long long to_int(StringView text, long long fallback) {
    long long v;
    return parse_int(text, v) ? v : fallback;
}

} // namespace osef
