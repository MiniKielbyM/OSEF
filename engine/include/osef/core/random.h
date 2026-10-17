#pragma once
// OSEF core: deterministic pseudo-random numbers (splitmix64). Fast and
// well distributed; not cryptographically secure.

#include <osef/core/base.h>

namespace osef {

class Random {
public:
    explicit constexpr Random(uint64_t seed) noexcept : state_(seed) {}

    constexpr uint64_t next() noexcept {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n) without modulo bias; returns 0 when n == 0.
    constexpr uint64_t below(uint64_t n) noexcept {
        if (n == 0) return 0;
        const uint64_t threshold = (0 - n) % n; // 2^64 mod n
        for (;;) {
            const uint64_t r = next();
            if (r >= threshold) return r % n;
        }
    }

    // Uniform in [lo, hi] (inclusive).
    constexpr uint64_t between(uint64_t lo, uint64_t hi) noexcept {
        if (hi <= lo) return lo;
        const uint64_t span = hi - lo;
        return span == UINT64_MAX ? next() : lo + below(span + 1);
    }

    constexpr uint64_t seed_state() const noexcept { return state_; }

private:
    uint64_t state_;
};

} // namespace osef
