// MC146818-compatible CMOS wall clock (polled; no periodic IRQ required).

#include "cpu.h"
#include "internal.h"

namespace osef::bm {
namespace {

constexpr uint16_t kIndex = 0x70;
constexpr uint16_t kData = 0x71;
constexpr uint8_t kSeconds = 0x00;
constexpr uint8_t kMinutes = 0x02;
constexpr uint8_t kHours = 0x04;
constexpr uint8_t kDay = 0x07;
constexpr uint8_t kMonth = 0x08;
constexpr uint8_t kYear = 0x09;
constexpr uint8_t kStatusA = 0x0A;
constexpr uint8_t kStatusB = 0x0B;
constexpr uint8_t kCentury = 0x32;

uint8_t read_cmos(uint8_t reg) {
    const uint8_t previous = cpu::inb(kIndex);
    cpu::outb(kIndex, static_cast<uint8_t>((previous & 0x80) | (reg & 0x7F)));
    const uint8_t value = cpu::inb(kData);
    cpu::outb(kIndex, previous);
    return value;
}

uint8_t from_bcd(uint8_t value) { return static_cast<uint8_t>((value & 0x0F) + ((value >> 4) * 10)); }

struct RawTime {
    uint8_t second, minute, hour, day, month, year, century;
};

RawTime sample() {
    return {read_cmos(kSeconds), read_cmos(kMinutes), read_cmos(kHours), read_cmos(kDay),
            read_cmos(kMonth), read_cmos(kYear), read_cmos(kCentury)};
}

bool equal(const RawTime& a, const RawTime& b) {
    return a.second == b.second && a.minute == b.minute && a.hour == b.hour && a.day == b.day &&
           a.month == b.month && a.year == b.year && a.century == b.century;
}

} // namespace

bool rtc_read(RtcTime& time) {
    if (!(spec().devices & Hw::RTC)) return false;

    RawTime raw{};
    bool stable = false;
    for (uint32_t retry = 0; retry < 8; ++retry) {
        uint32_t spin = 0;
        while ((read_cmos(kStatusA) & 0x80) && spin++ < 100000) cpu::pause();
        if (spin >= 100000) return false;
        const RawTime first = sample();
        const RawTime second = sample();
        if (equal(first, second)) {
            raw = second;
            stable = true;
            break;
        }
    }
    if (!stable) return false;

    const uint8_t status_b = read_cmos(kStatusB);
    const bool binary = (status_b & 0x04) != 0;
    const bool hour_24 = (status_b & 0x02) != 0;
    const bool pm = (raw.hour & 0x80) != 0;
    raw.hour &= 0x7F;
    if (!binary) {
        raw.second = from_bcd(raw.second);
        raw.minute = from_bcd(raw.minute);
        raw.hour = from_bcd(raw.hour);
        raw.day = from_bcd(raw.day);
        raw.month = from_bcd(raw.month);
        raw.year = from_bcd(raw.year);
        raw.century = from_bcd(raw.century);
    }
    if (raw.century > 99) raw.century = 0;
    if (!hour_24) {
        if (raw.hour < 1 || raw.hour > 12) return false;
        raw.hour = static_cast<uint8_t>((raw.hour % 12) + (pm ? 12 : 0));
    }
    if (raw.month < 1 || raw.month > 12 || raw.day < 1 || raw.day > 31 || raw.hour > 23 ||
        raw.minute > 59 || raw.second > 60)
        return false;

    const uint16_t year = raw.century ? static_cast<uint16_t>(raw.century * 100 + raw.year)
                                      : static_cast<uint16_t>(raw.year < 70 ? 2000 + raw.year : 1900 + raw.year);
    if (year < 1970 || year > 9999) return false;
    time = {year, raw.month, raw.day, raw.hour, raw.minute, raw.second};
    return true;
}

} // namespace osef::bm
