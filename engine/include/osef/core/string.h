#pragma once
// OSEF core: StringView (non-owning) and String (owning, NUL-terminated).
//
// Positions are byte offsets. Out-of-range positions never fault: substr,
// erase and insert clamp `pos` to size(); the find family returns npos.
// Element access (operator[], front, back, pop_back) asserts its bounds.

#include <osef/core/base.h>

namespace osef {

class String;

// ---------------------------------------------------------------------------
// StringView
// ---------------------------------------------------------------------------
class StringView {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    constexpr StringView() noexcept : data_(""), size_(0) {}
    // A null pointer is treated as the empty string.
    constexpr StringView(const char* s) noexcept
        : data_(s ? s : ""), size_(s ? detail::cstr_length(s) : 0) {}
    constexpr StringView(const char* s, size_t n) noexcept : data_(s ? s : ""), size_(s ? n : 0) {}

    constexpr const char* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr size_t length() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr const char* begin() const noexcept { return data_; }
    constexpr const char* end() const noexcept { return data_ + size_; }

    const char& operator[](size_t i) const {
        OSEF_ASSERT(i < size_);
        return data_[i];
    }
    const char& front() const {
        OSEF_ASSERT(size_ > 0);
        return data_[0];
    }
    const char& back() const {
        OSEF_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }

    void remove_prefix(size_t n) noexcept {
        if (n > size_) n = size_;
        data_ += n;
        size_ -= n;
    }
    void remove_suffix(size_t n) noexcept {
        if (n > size_) n = size_;
        size_ -= n;
    }

    StringView substr(size_t pos, size_t n = npos) const noexcept {
        if (pos > size_) pos = size_;
        if (n > size_ - pos) n = size_ - pos;
        return StringView(data_ + pos, n);
    }

    int compare(StringView other) const noexcept {
        const size_t n = size_ < other.size_ ? size_ : other.size_;
        if (n != 0) {
            const int c = detail::mem_compare(data_, other.data_, n);
            if (c != 0) return c < 0 ? -1 : 1;
        }
        if (size_ == other.size_) return 0;
        return size_ < other.size_ ? -1 : 1;
    }

    bool starts_with(StringView p) const noexcept {
        return p.size_ <= size_ && (p.size_ == 0 || detail::mem_compare(data_, p.data_, p.size_) == 0);
    }
    bool starts_with(char c) const noexcept { return size_ > 0 && data_[0] == c; }
    bool ends_with(StringView s) const noexcept {
        return s.size_ <= size_ &&
               (s.size_ == 0 || detail::mem_compare(data_ + size_ - s.size_, s.data_, s.size_) == 0);
    }
    bool ends_with(char c) const noexcept { return size_ > 0 && data_[size_ - 1] == c; }
    bool contains(StringView s) const noexcept { return find(s) != npos; }
    bool contains(char c) const noexcept { return find(c) != npos; }

    size_t find(char c, size_t pos = 0) const noexcept {
        for (size_t i = pos; i < size_; ++i)
            if (data_[i] == c) return i;
        return npos;
    }
    size_t find(StringView s, size_t pos = 0) const noexcept {
        if (pos > size_ || s.size_ > size_ - pos) return npos;
        if (s.size_ == 0) return pos;
        const size_t last = size_ - s.size_;
        for (size_t i = pos; i <= last; ++i) {
            if (data_[i] == s.data_[0] && detail::mem_compare(data_ + i, s.data_, s.size_) == 0) return i;
        }
        return npos;
    }
    // Last occurrence starting at or before pos.
    size_t rfind(char c, size_t pos = npos) const noexcept {
        if (size_ == 0) return npos;
        size_t i = pos < size_ ? pos : size_ - 1;
        for (;;) {
            if (data_[i] == c) return i;
            if (i == 0) return npos;
            --i;
        }
    }
    size_t rfind(StringView s, size_t pos = npos) const noexcept {
        if (s.size_ > size_) return npos;
        size_t i = size_ - s.size_;
        if (pos < i) i = pos;
        for (;;) {
            if (s.size_ == 0 || detail::mem_compare(data_ + i, s.data_, s.size_) == 0) return i;
            if (i == 0) return npos;
            --i;
        }
    }
    size_t find_first_of(StringView set, size_t pos = 0) const noexcept {
        for (size_t i = pos; i < size_; ++i)
            if (set.find(data_[i]) != npos) return i;
        return npos;
    }
    size_t find_first_not_of(StringView set, size_t pos = 0) const noexcept {
        for (size_t i = pos; i < size_; ++i)
            if (set.find(data_[i]) == npos) return i;
        return npos;
    }
    size_t find_last_of(StringView set, size_t pos = npos) const noexcept {
        for (size_t i = (pos < size_ ? pos + 1 : size_); i > 0; --i)
            if (set.find(data_[i - 1]) != npos) return i - 1;
        return npos;
    }
    size_t find_last_not_of(StringView set, size_t pos = npos) const noexcept {
        for (size_t i = (pos < size_ ? pos + 1 : size_); i > 0; --i)
            if (set.find(data_[i - 1]) == npos) return i - 1;
        return npos;
    }

    // Removes leading/trailing spaces, tabs, CR and LF.
    StringView trimmed() const noexcept {
        const StringView ws(" \t\r\n", 4);
        const size_t b = find_first_not_of(ws);
        if (b == npos) return StringView(data_ + size_, 0);
        const size_t e = find_last_not_of(ws);
        return StringView(data_ + b, e - b + 1);
    }

private:
    const char* data_;
    size_t size_;
};

inline bool operator==(StringView a, StringView b) noexcept {
    return a.size() == b.size() && (a.size() == 0 || detail::mem_compare(a.data(), b.data(), a.size()) == 0);
}
inline bool operator!=(StringView a, StringView b) noexcept { return !(a == b); }
inline bool operator<(StringView a, StringView b) noexcept { return a.compare(b) < 0; }
inline bool operator>(StringView a, StringView b) noexcept { return a.compare(b) > 0; }
inline bool operator<=(StringView a, StringView b) noexcept { return a.compare(b) <= 0; }
inline bool operator>=(StringView a, StringView b) noexcept { return a.compare(b) >= 0; }

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------
// Owning byte string, always NUL-terminated (c_str() == data()). Strings of
// up to 15 bytes are stored inline without a heap allocation. A moved-from
// String is empty.
class String {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    using value_type = char;
    using iterator = char*;
    using const_iterator = const char*;

    String() noexcept : ptr_(local_), size_(0), cap_(kLocalCap) { local_[0] = '\0'; }
    String(const char* s) : String() { assign(StringView(s)); }
    String(const char* s, size_t n) : String() { assign(s, n); }
    explicit String(StringView sv) : String() { assign(sv.data(), sv.size()); }
    String(size_t n, char c) : String() { append(n, c); }
    String(const String& other) : String() { assign(other.ptr_, other.size_); }
    String(String&& other) noexcept : String() { take(other); }
    ~String() { release_heap(); }

    String& operator=(const String& other) {
        if (this != &other) assign(other.ptr_, other.size_);
        return *this;
    }
    String& operator=(String&& other) noexcept {
        if (this != &other) {
            release_heap();
            ptr_ = local_;
            cap_ = kLocalCap;
            take(other);
        }
        return *this;
    }
    String& operator=(const char* s) { return assign(StringView(s)); }
    String& operator=(StringView sv) { return assign(sv.data(), sv.size()); }

    // `s` may point into this string.
    String& assign(const char* s, size_t n);
    String& assign(StringView sv) { return assign(sv.data(), sv.size()); }

    const char* c_str() const noexcept { return ptr_; }
    const char* data() const noexcept { return ptr_; }
    // Writable buffer of size()+1 bytes; data()[size()] must stay '\0'.
    char* data() noexcept { return ptr_; }
    size_t size() const noexcept { return size_; }
    size_t length() const noexcept { return size_; }
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }

    char* begin() noexcept { return ptr_; }
    char* end() noexcept { return ptr_ + size_; }
    const char* begin() const noexcept { return ptr_; }
    const char* end() const noexcept { return ptr_ + size_; }

    // Index size() is allowed and refers to the terminator.
    char& operator[](size_t i) {
        OSEF_ASSERT(i <= size_);
        return ptr_[i];
    }
    const char& operator[](size_t i) const {
        OSEF_ASSERT(i <= size_);
        return ptr_[i];
    }
    char& front() {
        OSEF_ASSERT(size_ > 0);
        return ptr_[0];
    }
    const char& front() const {
        OSEF_ASSERT(size_ > 0);
        return ptr_[0];
    }
    char& back() {
        OSEF_ASSERT(size_ > 0);
        return ptr_[size_ - 1];
    }
    const char& back() const {
        OSEF_ASSERT(size_ > 0);
        return ptr_[size_ - 1];
    }

    void reserve(size_t n);
    void shrink_to_fit();
    void resize(size_t n, char c = '\0');
    void clear() noexcept {
        size_ = 0;
        ptr_[0] = '\0';
    }
    void push_back(char c) { append(&c, 1); }
    void pop_back() {
        OSEF_ASSERT(size_ > 0);
        ptr_[--size_] = '\0';
    }

    // `s` may point into this string.
    String& append(const char* s, size_t n);
    String& append(StringView sv) { return append(sv.data(), sv.size()); }
    String& append(const char* s) { return append(StringView(s)); }
    String& append(size_t n, char c);

    String& operator+=(const String& s) { return append(s.ptr_, s.size_); }
    String& operator+=(StringView sv) { return append(sv.data(), sv.size()); }
    String& operator+=(const char* s) { return append(StringView(s)); }
    String& operator+=(char c) { return append(&c, 1); }

    // Inserts at pos (clamped to size()); `sv` may point into this string.
    String& insert(size_t pos, StringView sv);
    String& insert(size_t pos, size_t n, char c);
    // Removes up to n characters at pos (clamped).
    String& erase(size_t pos = 0, size_t n = npos);
    // Replaces up to n characters at pos (clamped) with sv.
    String& replace(size_t pos, size_t n, StringView sv);

    String substr(size_t pos = 0, size_t n = npos) const { return String(view().substr(pos, n)); }

    size_t find(char c, size_t pos = 0) const noexcept { return view().find(c, pos); }
    size_t find(StringView s, size_t pos = 0) const noexcept { return view().find(s, pos); }
    size_t rfind(char c, size_t pos = npos) const noexcept { return view().rfind(c, pos); }
    size_t rfind(StringView s, size_t pos = npos) const noexcept { return view().rfind(s, pos); }
    size_t find_first_of(StringView set, size_t pos = 0) const noexcept { return view().find_first_of(set, pos); }
    size_t find_first_not_of(StringView set, size_t pos = 0) const noexcept {
        return view().find_first_not_of(set, pos);
    }
    size_t find_last_of(StringView set, size_t pos = npos) const noexcept { return view().find_last_of(set, pos); }
    size_t find_last_not_of(StringView set, size_t pos = npos) const noexcept {
        return view().find_last_not_of(set, pos);
    }
    bool starts_with(StringView p) const noexcept { return view().starts_with(p); }
    bool starts_with(char c) const noexcept { return view().starts_with(c); }
    bool ends_with(StringView s) const noexcept { return view().ends_with(s); }
    bool ends_with(char c) const noexcept { return view().ends_with(c); }
    bool contains(StringView s) const noexcept { return view().contains(s); }
    bool contains(char c) const noexcept { return view().contains(c); }
    int compare(StringView other) const noexcept { return view().compare(other); }

    StringView view() const noexcept { return StringView(ptr_, size_); }
    operator StringView() const noexcept { return StringView(ptr_, size_); }

    void swap(String& other) noexcept;

private:
    static constexpr size_t kLocalCap = 15;

    bool is_local() const noexcept { return ptr_ == local_; }
    void release_heap() noexcept;
    void take(String& other) noexcept; // requires *this to be empty and local
    // Moves to a buffer of capacity new_cap (>= size_), keeping contents.
    void reallocate(size_t new_cap);
    size_t grown_capacity(size_t needed) const;

    char* ptr_;
    size_t size_;
    size_t cap_; // usable bytes, excluding the terminator
    char local_[kLocalCap + 1];
};

// osef::swap(a, b) (base.h) uses String::swap.

// Concatenation for every combination of String / StringView / const char* /
// char. Rvalue String left operands are reused.
String operator+(const String& a, const String& b);
String operator+(const String& a, StringView b);
String operator+(StringView a, const String& b);
String operator+(const String& a, const char* b);
String operator+(const char* a, const String& b);
String operator+(const String& a, char b);
String operator+(char a, const String& b);
String operator+(StringView a, StringView b);
String operator+(StringView a, const char* b);
String operator+(const char* a, StringView b);
String operator+(StringView a, char b);
String operator+(char a, StringView b);
String operator+(String&& a, const String& b);
String operator+(String&& a, StringView b);
String operator+(String&& a, const char* b);
String operator+(String&& a, char b);

// Decimal conversions.
String to_string(int v);
String to_string(unsigned v);
String to_string(long v);
String to_string(unsigned long v);
String to_string(long long v);
String to_string(unsigned long long v);

// Parses an optional sign followed by decimal digits, or 0x/0X and hex
// digits. The whole input must match (no whitespace). Returns false (and
// leaves `out` unchanged) on empty input, junk, or overflow.
bool parse_int(StringView text, long long& out);
// parse_int, or `fallback` when the text is not a valid integer.
long long to_int(StringView text, long long fallback = 0);

} // namespace osef
