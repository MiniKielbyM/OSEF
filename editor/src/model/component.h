#pragma once
// OS components shown in the Hierarchy: hardware (timer, keyboard, USB, ...),
// kernel subsystems and userland. A component is plain data plus children;
// the catalog of known components lives in model/component_catalog.cpp.

#include "model/property.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

enum class ComponentKind {
    Os,          // hierarchy root: the operating system itself
    Group,       // folder-like grouping ("Hardware", "Kernel", ...)
    Firmware, Cpu, Memory, Interrupts, Timer, Clock, Serial, Display, Keyboard, Mouse,
    Usb, Storage, Pci, Network, Audio, Power,
    Scheduler, Processes, Ipc, Syscalls, FileSystem, Devices,
    Shell, Programs, Program, Modules, Module,
};
const char* to_string(ComponentKind kind);

enum class ComponentStatus {
    Implemented,  // works in the engine today
    Partial,      // some of it works
    Planned,      // not in the engine yet: a place to expand
};
const char* to_string(ComponentStatus status);

// A source file that implements or configures a component.
struct SourceRef {
    enum class Base { Engine, Project };
    Base base = Base::Engine;  // Engine: relative to the OSEF checkout; Project: relative to the OS directory
    std::string path;          // e.g. "engine/src/baremetal/keyboard.cpp" or "main.cpp"
    std::string label;         // e.g. "PS/2 keyboard driver"
    int line = 0;              // 1-based line hint, 0 = top
};

struct Component {
    std::string id;           // stable dotted id, e.g. "hw.keyboard"
    std::string name;         // "Keyboard"
    std::string summary;      // one line under the name in the Inspector
    std::string description;  // Documentation section; paragraphs separated by "\n\n"
    ComponentKind kind = ComponentKind::Group;
    ComponentStatus status = ComponentStatus::Implemented;

    // Unity-style enable checkbox (Inspector header + Hierarchy). For hardware
    // devices this is bound to the device flag in OSEF_HARDWARE.
    bool toggleable = false;
    bool enabled = true;
    std::string toggle_tooltip;

    std::vector<Property> properties;
    std::vector<SourceRef> sources;   // sources.front() is opened when selected

    Component* parent = nullptr;
    std::vector<std::unique_ptr<Component>> children;

    Component& add_child(std::unique_ptr<Component> child);  // sets child->parent
    Component* find(std::string_view component_id);          // depth-first, includes this
    const Component* find(std::string_view component_id) const;
    Property* property(std::string_view property_id);
    const Property* property(std::string_view property_id) const;
    std::string path() const;                                // "MyOS / Hardware / Keyboard"
};

} // namespace osef::editor
