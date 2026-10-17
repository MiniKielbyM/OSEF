#pragma once
// OSEF core: printf-style formatting without a C library.
//
// Conversions: %d %i %u %x %X %o %c %s %p %%
// Flags: - 0 + space #   Width and .precision (digits or *).
// Length modifiers: hh h l ll z j t (L accepted).
// Floating point is not supported: %f %F %e %E %g %G %a %A consume their
// double argument and print "?". %n consumes its pointer argument but writes
// nothing (no writes through format arguments). An unknown or
// incomplete conversion is copied to the output verbatim.
// %s with a null pointer prints "(null)"; %p prints 0x followed by hex
// digits (0x0 for null).

#include <stdarg.h>

#include <osef/core/base.h>
#include <osef/core/string.h>

namespace osef {

// snprintf semantics: writes at most n-1 characters plus a terminating NUL
// (always terminates when n > 0; buf may be null when n == 0) and returns
// the length the full output would have had.
size_t vformat(char* buf, size_t n, const char* fmt, va_list args) OSEF_PRINTF_FORMAT(3, 0);
size_t format_to(char* buf, size_t n, const char* fmt, ...) OSEF_PRINTF_FORMAT(3, 4);

// Formats into a new String.
String vformat(const char* fmt, va_list args) OSEF_PRINTF_FORMAT(1, 0);
String format(const char* fmt, ...) OSEF_PRINTF_FORMAT(1, 2);

} // namespace osef
