// OSEF core: printf-style formatting (see osef/core/format.h).

#include <osef/core/format.h>

namespace osef {

namespace {

// Bounded output that keeps counting past the end (snprintf semantics).
struct Sink {
    char* buf;
    size_t cap;
    size_t len;

    size_t room() const { return len + 1 < cap ? cap - 1 - len : 0; }
    void put(char c) {
        if (len + 1 < cap) buf[len] = c;
        ++len;
    }
    void put_n(char c, size_t n) {
        const size_t r = room();
        if (r) detail::mem_set(buf + len, static_cast<unsigned char>(c), n < r ? n : r);
        len += n;
    }
    void write(const char* s, size_t n) {
        const size_t r = room();
        if (r && n) detail::mem_copy(buf + len, s, n < r ? n : r);
        len += n;
    }
};

struct Spec {
    bool left = false;
    bool plus = false;
    bool space = false;
    bool alt = false;
    bool zero = false;
    bool has_prec = false;
    size_t width = 0;
    size_t prec = 0;
};

enum class Length { None, HH, H, L, LL, Z, J, T, BigL };

constexpr size_t kMaxField = static_cast<size_t>(INT_MAX);

void emit_text(Sink& out, const Spec& sp, const char* s, size_t n) {
    const size_t pad = sp.width > n ? sp.width - n : 0;
    if (!sp.left) out.put_n(' ', pad);
    out.write(s, n);
    if (sp.left) out.put_n(' ', pad);
}

// value: magnitude; sign: '-', '+', ' ' or 0; force_prefix: always "0x" (%p).
void emit_integer(Sink& out, const Spec& sp, unsigned long long value, char sign, unsigned base, bool upper,
                  bool force_prefix) {
    const char* digit_chars = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[24]; // least significant first; 22 octal digits max
    size_t nd = 0;
    if (!(value == 0 && sp.has_prec && sp.prec == 0)) {
        unsigned long long v = value;
        do {
            digits[nd++] = digit_chars[v % base];
            v /= base;
        } while (v != 0);
    }
    size_t zeros = sp.has_prec && sp.prec > nd ? sp.prec - nd : 0;
    // '#' with o: the first digit must be 0.
    if (base == 8 && sp.alt && zeros == 0 && (nd == 0 || digits[nd - 1] != '0')) zeros = 1;

    char prefix[3];
    size_t np = 0;
    if (sign) prefix[np++] = sign;
    if (base == 16 && (force_prefix || (sp.alt && value != 0))) {
        prefix[np++] = '0';
        prefix[np++] = upper ? 'X' : 'x';
    }

    const size_t body = np + zeros + nd;
    size_t pad = sp.width > body ? sp.width - body : 0;
    if (!sp.left) {
        if (sp.zero && !sp.has_prec) {
            zeros += pad; // zero padding goes between prefix and digits
        } else {
            out.put_n(' ', pad);
        }
        pad = 0;
    }
    out.write(prefix, np);
    out.put_n('0', zeros);
    while (nd > 0) out.put(digits[--nd]);
    out.put_n(' ', pad);
}

size_t parse_number(const char*& p) {
    size_t v = 0;
    while (*p >= '0' && *p <= '9') {
        const size_t d = static_cast<size_t>(*p - '0');
        v = v > (kMaxField - d) / 10 ? kMaxField : v * 10 + d;
        ++p;
    }
    return v;
}

size_t bounded_length(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') ++n;
    return n;
}

} // namespace

size_t vformat(char* buf, size_t n, const char* fmt, va_list args) {
    Sink out{buf, buf ? n : 0, 0};
    va_list ap;
    va_copy(ap, args);
    const char* p = fmt ? fmt : "";

    while (*p != '\0') {
        if (*p != '%') {
            const char* q = p;
            while (*q != '\0' && *q != '%') ++q;
            out.write(p, static_cast<size_t>(q - p));
            p = q;
            continue;
        }
        const char* start = p++;
        Spec sp;

        for (;; ++p) {
            if (*p == '-') sp.left = true;
            else if (*p == '+') sp.plus = true;
            else if (*p == ' ') sp.space = true;
            else if (*p == '#') sp.alt = true;
            else if (*p == '0') sp.zero = true;
            else break;
        }

        if (*p == '*') {
            const int w = va_arg(ap, int);
            if (w < 0) {
                sp.left = true;
                sp.width = 0u - static_cast<unsigned>(w);
            } else {
                sp.width = static_cast<size_t>(w);
            }
            ++p;
        } else {
            sp.width = parse_number(p);
        }

        if (*p == '.') {
            ++p;
            sp.has_prec = true;
            if (*p == '*') {
                const int pr = va_arg(ap, int);
                if (pr < 0) sp.has_prec = false; // negative precision: as if omitted
                else sp.prec = static_cast<size_t>(pr);
                ++p;
            } else {
                sp.prec = parse_number(p);
            }
        }

        Length len = Length::None;
        switch (*p) {
        case 'h':
            ++p;
            if (*p == 'h') {
                ++p;
                len = Length::HH;
            } else {
                len = Length::H;
            }
            break;
        case 'l':
            ++p;
            if (*p == 'l') {
                ++p;
                len = Length::LL;
            } else {
                len = Length::L;
            }
            break;
        case 'q': ++p; len = Length::LL; break;
        case 'z': ++p; len = Length::Z; break;
        case 'j': ++p; len = Length::J; break;
        case 't': ++p; len = Length::T; break;
        case 'L': ++p; len = Length::BigL; break;
        default: break;
        }

        const char conv = *p;
        if (conv == '\0') { // incomplete conversion: copy it verbatim
            out.write(start, static_cast<size_t>(p - start));
            break;
        }
        ++p;

        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            switch (len) {
            case Length::HH: v = static_cast<signed char>(va_arg(ap, int)); break;
            case Length::H: v = static_cast<short>(va_arg(ap, int)); break;
            case Length::L: v = va_arg(ap, long); break;
            case Length::LL: v = va_arg(ap, long long); break;
            case Length::Z: v = static_cast<ptrdiff_t>(va_arg(ap, size_t)); break;
            case Length::J: v = static_cast<long long>(va_arg(ap, intmax_t)); break;
            case Length::T: v = va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, int); break;
            }
            const unsigned long long mag =
                v < 0 ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
            const char sign = v < 0 ? '-' : sp.plus ? '+' : sp.space ? ' ' : '\0';
            emit_integer(out, sp, mag, sign, 10, false, false);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v;
            switch (len) {
            case Length::HH: v = static_cast<unsigned char>(va_arg(ap, unsigned)); break;
            case Length::H: v = static_cast<unsigned short>(va_arg(ap, unsigned)); break;
            case Length::L: v = va_arg(ap, unsigned long); break;
            case Length::LL: v = va_arg(ap, unsigned long long); break;
            case Length::Z: v = va_arg(ap, size_t); break;
            case Length::J: v = static_cast<unsigned long long>(va_arg(ap, uintmax_t)); break;
            case Length::T: v = static_cast<size_t>(va_arg(ap, ptrdiff_t)); break;
            default: v = va_arg(ap, unsigned); break;
            }
            const unsigned base = conv == 'u' ? 10u : conv == 'o' ? 8u : 16u;
            emit_integer(out, sp, v, '\0', base, conv == 'X', false);
            break;
        }
        case 'p': {
            const void* v = va_arg(ap, void*);
            emit_integer(out, sp, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v)), '\0', 16,
                         false, true);
            break;
        }
        case 'c': {
            const char c = static_cast<char>(va_arg(ap, int));
            Spec text = sp;
            text.has_prec = false;
            emit_text(out, text, &c, 1);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            const size_t n_chars = sp.has_prec ? bounded_length(s, sp.prec) : detail::cstr_length(s);
            emit_text(out, sp, s, n_chars);
            break;
        }
        case '%': { // flags/width are undefined for %% in C; honour the width like BSD libc
            Spec text = sp;
            text.has_prec = false;
            emit_text(out, text, "%", 1);
            break;
        }
        case 'n': // not supported (never writes through the pointer), but consume it
            (void)va_arg(ap, void*);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            // No floating point support: consume the argument, print "?".
            if (len == Length::BigL) (void)va_arg(ap, long double);
            else (void)va_arg(ap, double);
            Spec text = sp;
            text.has_prec = false;
            emit_text(out, text, "?", 1);
            break;
        }
        default: // unknown conversion: copy verbatim
            out.write(start, static_cast<size_t>(p - start));
            break;
        }
    }
    va_end(ap);

    if (out.cap > 0) out.buf[out.len < out.cap ? out.len : out.cap - 1] = '\0';
    return out.len;
}

size_t format_to(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t len = vformat(buf, n, fmt, ap);
    va_end(ap);
    return len;
}

String vformat(const char* fmt, va_list args) {
    va_list again;
    va_copy(again, args);
    char small[256];
    const size_t len = vformat(small, sizeof(small), fmt, args);
    String s;
    if (len < sizeof(small)) {
        s.assign(small, len);
    } else {
        s.resize(len);
        vformat(s.data(), len + 1, fmt, again); // String keeps room for the NUL
    }
    va_end(again);
    return s;
}

String format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    String s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

} // namespace osef
