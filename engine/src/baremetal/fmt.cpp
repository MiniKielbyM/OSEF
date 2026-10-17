// Minimal printf-style formatter for the bare-metal layer's log / panic paths.
// Deliberately independent of the osef core library.

#include "internal.h"

namespace osef::bm {

namespace {

struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    void put(char c) {
        if (len + 1 < cap) buf[len] = c;
        ++len;
    }
};

void put_padded(Out& o, const char* digits, size_t ndigits, const char* prefix, size_t nprefix, int width,
                bool zero, bool left) {
    size_t total = ndigits + nprefix;
    size_t pad = (width > 0 && static_cast<size_t>(width) > total) ? static_cast<size_t>(width) - total : 0;
    if (!left && !zero)
        for (size_t i = 0; i < pad; ++i) o.put(' ');
    for (size_t i = 0; i < nprefix; ++i) o.put(prefix[i]);
    if (!left && zero)
        for (size_t i = 0; i < pad; ++i) o.put('0');
    for (size_t i = 0; i < ndigits; ++i) o.put(digits[i]);
    if (left)
        for (size_t i = 0; i < pad; ++i) o.put(' ');
}

size_t utoa(uint64_t v, unsigned base, bool upper, char* tmp /* >= 24 */) {
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[24];
    size_t n = 0;
    do {
        rev[n++] = set[v % base];
        v /= base;
    } while (v != 0);
    for (size_t i = 0; i < n; ++i) tmp[i] = rev[n - 1 - i];
    return n;
}

} // namespace

size_t cstr_len(const char* s) {
    size_t n = 0;
    if (s)
        while (s[n]) ++n;
    return n;
}

void copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < dst_size; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

size_t vfmt(char* buf, size_t n, const char* f, va_list ap) {
    Out o{buf, n};
    while (*f) {
        if (*f != '%') {
            o.put(*f++);
            continue;
        }
        ++f;
        bool zero = false, left = false;
        for (;; ++f) {
            if (*f == '0') zero = true;
            else if (*f == '-') left = true;
            else break;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int lng = 0;  // 0 int, 1 long, 2 long long, 3 size_t
        if (*f == 'l') {
            ++f;
            lng = 1;
            if (*f == 'l') {
                ++f;
                lng = 2;
            }
        } else if (*f == 'z') {
            ++f;
            lng = 3;
        }
        char conv = *f ? *f++ : '\0';
        char tmp[24];
        switch (conv) {
        case 'd':
        case 'i': {
            long long v = lng == 0 ? va_arg(ap, int)
                        : lng == 1 ? va_arg(ap, long)
                        : lng == 2 ? va_arg(ap, long long)
                                   : static_cast<long long>(va_arg(ap, size_t));
            uint64_t mag = v < 0 ? (0 - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
            size_t nd = utoa(mag, 10, false, tmp);
            put_padded(o, tmp, nd, "-", v < 0 ? 1 : 0, width, zero, left);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng == 0 ? va_arg(ap, unsigned)
                       : lng == 1 ? va_arg(ap, unsigned long)
                       : lng == 2 ? va_arg(ap, unsigned long long)
                                  : va_arg(ap, size_t);
            size_t nd = utoa(v, conv == 'u' ? 10 : 16, conv == 'X', tmp);
            put_padded(o, tmp, nd, "", 0, width, zero, left);
            break;
        }
        case 'p': {
            uint64_t v = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
            size_t nd = utoa(v, 16, false, tmp);
            put_padded(o, tmp, nd, "0x", 2, 18, true, false);
            break;
        }
        case 'c': {
            char c = static_cast<char>(va_arg(ap, int));
            put_padded(o, &c, 1, "", 0, width, false, left);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            put_padded(o, s, cstr_len(s), "", 0, width, false, left);
            break;
        }
        case '%': o.put('%'); break;
        case '\0': break;
        default:
            o.put('%');
            o.put(conv);
            break;
        }
    }
    if (n > 0) buf[o.len < n ? o.len : n - 1] = '\0';
    return o.len;
}

size_t fmt(char* buf, size_t n, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    size_t r = vfmt(buf, n, f, ap);
    va_end(ap);
    return r;
}

void log(const char* f, ...) {
    char line[512];
    size_t pre = fmt(line, sizeof line, "[osef] ");
    va_list ap;
    va_start(ap, f);
    size_t len = vfmt(line + pre, sizeof line - pre - 1, f, ap);  // keep room for '\n'
    va_end(ap);
    size_t total = pre + len;
    if (total > sizeof line - 2) total = sizeof line - 2;
    line[total++] = '\n';
    line[total] = '\0';
    console_write(line, total);
}

} // namespace osef::bm
