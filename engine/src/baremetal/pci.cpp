// PCI configuration mechanism #1 enumeration for legacy x86 PCI/PCIe hosts.

#include "cpu.h"
#include "internal.h"

namespace osef::bm {
namespace {

constexpr uint16_t kConfigAddress = 0xCF8;
constexpr uint16_t kConfigData = 0xCFC;
constexpr size_t kMaxDevices = 256;

uint32_t config_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    const uint32_t address = 0x80000000u | (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(device) << 11) |
                             (static_cast<uint32_t>(function) << 8) | (offset & 0xFC);
    cpu::outl(kConfigAddress, address);
    return cpu::inl(kConfigData);
}

void config_write(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    const uint32_t address = 0x80000000u | (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(device) << 11) |
                             (static_cast<uint32_t>(function) << 8) | (offset & 0xFC);
    cpu::outl(kConfigAddress, address);
    cpu::outl(kConfigData, value);
}

} // namespace

uint32_t pci_read32(const PciInfo& d, uint8_t offset) { return config_read(d.bus, d.device, d.function, offset); }
void pci_write32(const PciInfo& d, uint8_t offset, uint32_t value) {
    config_write(d.bus, d.device, d.function, offset, value);
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, PciInfo& out, size_t index) {
    size_t seen = 0;
    for (uint32_t bus = 0; bus < 256; ++bus)
        for (uint8_t device = 0; device < 32; ++device) {
            const uint32_t id0 = config_read(static_cast<uint8_t>(bus), device, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF || (id0 & 0xFFFF) == 0) continue;
            const uint32_t header = config_read(static_cast<uint8_t>(bus), device, 0, 0x0C);
            const uint8_t functions = (static_cast<uint8_t>(header >> 16) & 0x80) ? 8 : 1;
            for (uint8_t function = 0; function < functions; ++function) {
                const uint32_t id = function == 0 ? id0 : config_read(static_cast<uint8_t>(bus), device, function, 0);
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) continue;
                const uint32_t c = config_read(static_cast<uint8_t>(bus), device, function, 0x08);
                if (static_cast<uint8_t>(c >> 24) != class_code || static_cast<uint8_t>(c >> 16) != subclass ||
                    static_cast<uint8_t>(c >> 8) != prog_if)
                    continue;
                if (seen++ != index) continue;
                out = {static_cast<uint8_t>(bus), device, function, static_cast<uint16_t>(id & 0xFFFF),
                       static_cast<uint16_t>(id >> 16), class_code, subclass, prog_if};
                return true;
            }
        }
    return false;
}

size_t pci_scan(PciInfo* devices, size_t capacity) {
    if (!(spec().devices & Hw::PCI)) return 0;
    size_t count = 0;
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            const uint32_t id0 = config_read(static_cast<uint8_t>(bus), device, 0, 0);
            const uint16_t vendor = static_cast<uint16_t>(id0 & 0xFFFF);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;
            const uint32_t header = config_read(static_cast<uint8_t>(bus), device, 0, 0x0C);
            const uint8_t functions = (static_cast<uint8_t>(header >> 16) & 0x80) ? 8 : 1;
            for (uint8_t function = 0; function < functions; ++function) {
                const uint32_t id = function == 0 ? id0 : config_read(static_cast<uint8_t>(bus), device, function, 0);
                const uint16_t fn_vendor = static_cast<uint16_t>(id & 0xFFFF);
                if (fn_vendor == 0xFFFF || fn_vendor == 0x0000) continue;
                const uint32_t class_info = config_read(static_cast<uint8_t>(bus), device, function, 0x08);
                if (devices && count < capacity && count < kMaxDevices) {
                    devices[count] = {static_cast<uint8_t>(bus), device, function, fn_vendor,
                                      static_cast<uint16_t>(id >> 16), static_cast<uint8_t>(class_info >> 24),
                                      static_cast<uint8_t>(class_info >> 16), static_cast<uint8_t>(class_info >> 8)};
                }
                ++count;
            }
        }
    }
    return count;
}

} // namespace osef::bm
