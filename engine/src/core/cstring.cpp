// OSEF core: the C runtime memory/string functions the compiler may emit
// calls to (struct copies, zero-initialization, __builtin_mem*). Bare metal
// only; hosted builds use the C library. The file is empty when hosted.
//
// These must never be compiled into calls to themselves: the bare-metal
// build uses -ffreestanding (which disables turning loops into mem* calls),
// and the x86_64 paths use string instructions directly. Note for interrupt
// handlers: the forward copies rely on the ABI's DF=0 (ISR stubs should `cld`).

#include <osef/core/base.h>

#if !OSEF_HOSTED

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
#if defined(__x86_64__)
    __asm__ __volatile__("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
#else
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    while (n--) *d++ = *s++;
#endif
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    if (d == s || n == 0) return dst;
    const uintptr_t di = reinterpret_cast<uintptr_t>(d);
    const uintptr_t si = reinterpret_cast<uintptr_t>(s);
    if (di < si || di >= si + n) {
        // Forward copy is safe: destination starts before the source or
        // the ranges do not overlap.
#if defined(__x86_64__)
        __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
        while (n--) *d++ = *s++;
#endif
    } else {
        // Overlap with the destination after the source: copy backwards.
        // (Plain loop: avoids toggling the direction flag.)
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
#if defined(__x86_64__)
    __asm__ __volatile__("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
#else
    unsigned char* d = static_cast<unsigned char*>(dst);
    while (n--) *d++ = static_cast<unsigned char>(c);
#endif
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = static_cast<const unsigned char*>(a);
    const unsigned char* y = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

} // extern "C"

#endif // !OSEF_HOSTED
