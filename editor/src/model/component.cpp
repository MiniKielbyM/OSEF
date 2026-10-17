#include "model/component.h"
#include <utility>

namespace osef::editor {

const char* to_string(ComponentKind kind) {
    switch (kind) {
    case ComponentKind::Os: return "Operating System";
    case ComponentKind::Group: return "Group";
    case ComponentKind::Firmware: return "Firmware";
    case ComponentKind::Cpu: return "CPU";
    case ComponentKind::Memory: return "Memory";
    case ComponentKind::Interrupts: return "Interrupts";
    case ComponentKind::Timer: return "Timer";
    case ComponentKind::Clock: return "Clock";
    case ComponentKind::Serial: return "Serial";
    case ComponentKind::Display: return "Display";
    case ComponentKind::Keyboard: return "Keyboard";
    case ComponentKind::Mouse: return "Mouse";
    case ComponentKind::Usb: return "USB";
    case ComponentKind::Storage: return "Storage";
    case ComponentKind::Pci: return "PCI";
    case ComponentKind::Network: return "Network";
    case ComponentKind::Audio: return "Audio";
    case ComponentKind::Power: return "Power";
    case ComponentKind::Scheduler: return "Scheduler";
    case ComponentKind::Processes: return "Processes";
    case ComponentKind::Ipc: return "IPC";
    case ComponentKind::Syscalls: return "System Calls";
    case ComponentKind::FileSystem: return "File System";
    case ComponentKind::Devices: return "Devices";
    case ComponentKind::Shell: return "Shell";
    case ComponentKind::Programs: return "Programs";
    case ComponentKind::Program: return "Program";
    case ComponentKind::Modules: return "Modules";
    case ComponentKind::Module: return "Module";
    }
    return "Component";
}

const char* to_string(ComponentStatus status) {
    switch (status) {
    case ComponentStatus::Implemented: return "Implemented";
    case ComponentStatus::Partial: return "Partial";
    case ComponentStatus::Planned: return "Planned";
    }
    return "Unknown";
}

Component& Component::add_child(std::unique_ptr<Component> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return *children.back();
}

Component* Component::find(std::string_view component_id) {
    if (id == component_id) return this;
    for (auto& child : children)
        if (Component* hit = child->find(component_id)) return hit;
    return nullptr;
}

const Component* Component::find(std::string_view component_id) const {
    return const_cast<Component*>(this)->find(component_id);
}

Property* Component::property(std::string_view property_id) {
    for (Property& p : properties)
        if (p.id == property_id) return &p;
    return nullptr;
}

const Property* Component::property(std::string_view property_id) const {
    return const_cast<Component*>(this)->property(property_id);
}

std::string Component::path() const {
    std::string out = name;
    for (const Component* p = parent; p; p = p->parent) out = p->name + " / " + out;
    return out;
}

} // namespace osef::editor
