// ACPI table discovery, S5 sleep type lookup and FADT-controlled soft-off.

#include "cpu.h"
#include "internal.h"
#include "limine.h"

namespace {

constexpr uint32_t kMaxTableLength = 1024 * 1024;
uint16_t g_pm1a_control = 0;
uint16_t g_pm1b_control = 0;
uint16_t g_smi_command = 0;
uint8_t g_acpi_enable = 0;
uint8_t g_sleep_type_a = 0;  // SLP_TYPa for PM1a_CNT
uint8_t g_sleep_type_b = 0;  // SLP_TYPb for PM1b_CNT

uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read64(const uint8_t* p) { return read32(p) | (static_cast<uint64_t>(read32(p + 4)) << 32); }

uint8_t checksum(const uint8_t* p, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum = static_cast<uint8_t>(sum + p[i]);
    return sum;
}

const uint8_t* physical(uint64_t address) {
    const uint64_t offset = osef::bm::hhdm_offset();
    if (!offset) return nullptr;
    if (address >= offset) return reinterpret_cast<const uint8_t*>(address);
    if (address > UINT64_MAX - offset) return nullptr;
    return reinterpret_cast<const uint8_t*>(address + offset);
}

const uint8_t* table(uint64_t address, const char* signature) {
    const uint8_t* p = physical(address);
    if (!p) return nullptr;
    for (size_t i = 0; i < 4; ++i)
        if (p[i] != static_cast<uint8_t>(signature[i])) return nullptr;
    const uint32_t length = read32(p + 4);
    if (length < 36 || length > kMaxTableLength || checksum(p, length) != 0) return nullptr;
    return p;
}

bool aml_integer(const uint8_t* p, const uint8_t* end, uint8_t& bytes, uint64_t& value) {
    if (p >= end) return false;
    switch (p[0]) {
    case 0x00: bytes = 1; value = 0; return true;
    case 0x01: bytes = 1; value = 1; return true;
    case 0x0A:
        if (end - p < 2) return false;
        bytes = 2;
        value = p[1];
        return true;
    case 0x0B:
        if (end - p < 3) return false;
        bytes = 3;
        value = p[1] | (static_cast<uint64_t>(p[2]) << 8);
        return true;
    case 0x0C:
        if (end - p < 5) return false;
        bytes = 5;
        value = read32(p + 1);
        return true;
    case 0x0E:
        if (end - p < 9) return false;
        bytes = 9;
        value = read64(p + 1);
        return true;
    default: return false;
    }
}

bool aml_package_length(const uint8_t* p, const uint8_t* end, uint32_t& length, uint8_t& bytes) {
    if (p >= end) return false;
    const uint8_t lead = p[0];
    const uint8_t follow = lead >> 6;
    if (end - p < static_cast<ptrdiff_t>(follow) + 1) return false;
    bytes = static_cast<uint8_t>(follow + 1);
    if (follow == 0) {
        length = lead & 0x3F;
    } else {
        length = lead & 0x0F;
        for (uint8_t b = 0; b < follow; ++b)
            length |= static_cast<uint32_t>(p[b + 1]) << (4 + b * 8);
    }
    return length >= bytes;
}

bool find_s5(const uint8_t* aml, uint32_t length, uint8_t& sleep_type_a, uint8_t& sleep_type_b) {
    const uint8_t* const end = aml + length;
    for (uint32_t i = 0; i + 5 < length; ++i) {
        if (aml[i] != 0x08 || aml[i + 1] != '_' || aml[i + 2] != 'S' || aml[i + 3] != '5' ||
            aml[i + 4] != '_')
            continue;
        const uint8_t* pkg = aml + i + 5;
        uint32_t pkg_len = 0;
        uint8_t pkg_prefix = 0;
        if (!aml_package_length(pkg, end, pkg_len, pkg_prefix) ||
            pkg_len > static_cast<uint32_t>(end - pkg))
            continue;
        const uint8_t* const pkg_end = pkg + pkg_len;
        const uint8_t* body = pkg + pkg_prefix;
        if (pkg_end - body < 2 || body[0] != 0x12) continue;

        uint32_t package_len = 0;
        uint8_t package_prefix = 0;
        if (!aml_package_length(body + 1, pkg_end, package_len, package_prefix) ||
            package_len > static_cast<uint32_t>(pkg_end - (body + 1)))
            continue;
        const uint8_t* const package_end = body + 1 + package_len;
        const uint8_t* elements = body + 1 + package_prefix;
        if (elements >= package_end || *elements < 2) continue;
        ++elements;
        uint8_t first_len = 0, second_len = 0;
        uint64_t first = 0, second = 0;
        if (!aml_integer(elements, package_end, first_len, first) ||
            !aml_integer(elements + first_len, package_end, second_len, second))
            continue;
        (void)second_len;
        sleep_type_a = static_cast<uint8_t>(first & 7);
        sleep_type_b = static_cast<uint8_t>(second & 7);
        return true;
    }
    return false;
}

bool find_fadt(const uint8_t* root, uint32_t entry_size) {
    const uint32_t length = read32(root + 4);
    if (length < 36 || (length - 36) % entry_size != 0) return false;
    const uint32_t count = (length - 36) / entry_size;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = root + 36 + i * entry_size;
        const uint64_t address = entry_size == 8 ? read64(entry) : read32(entry);
        const uint8_t* fadt = table(address, "FACP");
        if (!fadt) continue;
        const uint32_t fadt_length = read32(fadt + 4);
        if (fadt_length < 90) continue;

        const uint32_t dsdt32 = read32(fadt + 40);
        const uint64_t dsdt64 = fadt_length >= 148 ? read64(fadt + 140) : 0;
        const uint8_t* dsdt = dsdt64 ? table(dsdt64, "DSDT") : nullptr;
        if (!dsdt && dsdt32) dsdt = table(dsdt32, "DSDT");
        if (!dsdt || !find_s5(dsdt + 36, read32(dsdt + 4) - 36, g_sleep_type_a, g_sleep_type_b)) continue;

        const uint8_t* gas = fadt + 172;
        const uint64_t extended_port =
            fadt_length >= 184 && gas[0] == 1 && gas[1] >= 16 && gas[3] == 0 ? read64(gas + 4) : 0;
        const uint32_t legacy_port = read32(fadt + 64);
        const uint64_t port = extended_port ? extended_port : legacy_port;
        if (port == 0 || port > 0xFFFF || fadt[89] < 2) continue;
        g_pm1a_control = static_cast<uint16_t>(port);
        if (fadt_length >= 68) {
            const uint32_t pm1b = read32(fadt + 68);
            if (pm1b && pm1b <= 0xFFFF) g_pm1b_control = static_cast<uint16_t>(pm1b);
        }
        const uint32_t smi_command = read32(fadt + 48);
        g_smi_command = smi_command <= 0xFFFF ? static_cast<uint16_t>(smi_command) : 0;
        g_acpi_enable = fadt[52];
        return true;
    }
    return false;
}

bool discover() {
    const uintptr_t address = osef::bm::rsdp_address();
    if (!address) return false;
    const uint8_t* rsdp = physical(address);
    if (!rsdp) return false;
    constexpr char signature[] = "RSD PTR ";
    for (size_t i = 0; i < 8; ++i)
        if (rsdp[i] != static_cast<uint8_t>(signature[i])) return false;
    if (checksum(rsdp, 20) != 0) return false;
    const uint8_t revision = rsdp[15];
    uint64_t root_address = read32(rsdp + 16);
    const char* root_signature = "RSDT";
    uint32_t entry_size = 4;
    if (revision >= 2) {
        const uint32_t rsdp_length = read32(rsdp + 20);
        if (rsdp_length < 36 || rsdp_length > 4096 || checksum(rsdp, rsdp_length) != 0) return false;
        const uint64_t xsdt = read64(rsdp + 24);
        if (xsdt) {
            root_address = xsdt;
            root_signature = "XSDT";
            entry_size = 8;
        }
    }
    const uint8_t* root = table(root_address, root_signature);
    return root && find_fadt(root, entry_size);
}

} // namespace

namespace osef::bm {

void acpi_init() {
    g_pm1a_control = 0;
    g_pm1b_control = 0;
    g_smi_command = 0;
    g_acpi_enable = 0;
    g_sleep_type_a = 0;
    g_sleep_type_b = 0;
    // Limine's HHDM (base revision 3) does not map reserved memory, and BIOS
    // machines keep the RSDP in the reserved 0xE0000-0xFFFFF ROM area. Reading
    // an unmapped table page-faults, so discovery runs fault-protected: a fault
    // just means "no ACPI soft-off" and power_off() uses its fallbacks.
    bool found = false;
    char fault[128];
    const bool clean = protected_call([](void* out) { *static_cast<bool*>(out) = discover(); }, &found, fault,
                                      sizeof(fault));
    if (!clean) {
        g_pm1a_control = 0;
        g_pm1b_control = 0;
        log("ACPI: tables are not mapped (%s); emulator power-off fallback remains available", fault);
    } else if (found) {
        log("ACPI: found validated FADT and S5 soft-off method");
    } else {
        log("ACPI: no usable S5 soft-off method found; emulator fallback remains available");
    }
}

bool acpi_power_off() {
    if (!g_pm1a_control) return false;
    if (!(cpu::inw(g_pm1a_control) & 1) && g_smi_command && g_acpi_enable) {
        cpu::outb(g_smi_command, g_acpi_enable);
        for (uint32_t spin = 0; spin < 1000000; ++spin) {
            if (cpu::inw(g_pm1a_control) & 1) break;
            cpu::pause();
        }
    }
    // PM1x_CNT: SLP_TYPx in bits 10-12, SLP_EN in bit 13.
    constexpr uint16_t kSleepEnable = 1u << 13;
    cpu::outw(g_pm1a_control, static_cast<uint16_t>((g_sleep_type_a << 10) | kSleepEnable));
    if (g_pm1b_control) cpu::outw(g_pm1b_control, static_cast<uint16_t>((g_sleep_type_b << 10) | kSleepEnable));
    for (uint32_t spin = 0; spin < 100000; ++spin) cpu::pause();
    // ACPI soft-off should not return. If firmware/emulation ignored the request,
    // let the caller try its known fallback power-off ports.
    return false;
}

} // namespace osef::bm
