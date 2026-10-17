// Component catalog (model/component_catalog.h) and Component helpers.

#include "test_main.h"

#include "model/component_catalog.h"

#include <filesystem>
#include <functional>
#include <set>
#include <string>

using namespace osef::editor;

namespace {

void walk(const Component& c, const std::function<void(const Component&)>& fn) {
    fn(c);
    for (const auto& child : c.children) walk(*child, fn);
}

struct Expected {
    const char* id;
    const char* name;
    ComponentKind kind;
    ComponentStatus status;
};

// The table in editor/ARCHITECTURE.md ("Component catalog").
const Expected kTable[] = {
    {"boot", "Boot", ComponentKind::Group, ComponentStatus::Implemented},
    {"boot.firmware", "Firmware", ComponentKind::Firmware, ComponentStatus::Implemented},
    {"hw", "Hardware", ComponentKind::Group, ComponentStatus::Implemented},
    {"hw.cpu", "CPU", ComponentKind::Cpu, ComponentStatus::Implemented},
    {"hw.memory", "Memory", ComponentKind::Memory, ComponentStatus::Implemented},
    {"hw.interrupts", "Interrupts", ComponentKind::Interrupts, ComponentStatus::Implemented},
    {"hw.timer", "Time", ComponentKind::Timer, ComponentStatus::Implemented},
    {"hw.rtc", "Real-Time Clock", ComponentKind::Clock, ComponentStatus::Implemented},
    {"hw.serial", "Serial Port", ComponentKind::Serial, ComponentStatus::Implemented},
    {"hw.display", "Display", ComponentKind::Display, ComponentStatus::Implemented},
    {"hw.keyboard", "Keyboard", ComponentKind::Keyboard, ComponentStatus::Implemented},
    {"hw.mouse", "Mouse", ComponentKind::Mouse, ComponentStatus::Planned},
    {"hw.usb", "USB", ComponentKind::Usb, ComponentStatus::Partial},
    {"hw.storage", "Storage", ComponentKind::Storage, ComponentStatus::Planned},
    {"hw.pci", "PCI", ComponentKind::Pci, ComponentStatus::Implemented},
    {"hw.network", "Network", ComponentKind::Network, ComponentStatus::Planned},
    {"hw.audio", "Audio", ComponentKind::Audio, ComponentStatus::Planned},
    {"hw.power", "Power", ComponentKind::Power, ComponentStatus::Implemented},
    {"kernel", "Kernel", ComponentKind::Group, ComponentStatus::Implemented},
    {"kernel.scheduler", "Scheduler", ComponentKind::Scheduler, ComponentStatus::Implemented},
    {"kernel.processes", "Processes", ComponentKind::Processes, ComponentStatus::Implemented},
    {"kernel.ipc", "IPC & Services", ComponentKind::Ipc, ComponentStatus::Implemented},
    {"kernel.syscalls", "System Calls", ComponentKind::Syscalls, ComponentStatus::Implemented},
    {"kernel.vfs", "File Systems", ComponentKind::FileSystem, ComponentStatus::Implemented},
    {"kernel.devices", "Devices (/dev)", ComponentKind::Devices, ComponentStatus::Implemented},
    {"user", "Userland", ComponentKind::Group, ComponentStatus::Implemented},
    {"user.shell", "Shell", ComponentKind::Shell, ComponentStatus::Implemented},
    {"user.programs", "Programs", ComponentKind::Programs, ComponentStatus::Implemented},
    {"user.modules", "Modules", ComponentKind::Modules, ComponentStatus::Implemented},
};

} // namespace

TEST(catalog_root_is_the_os) {
    auto root = build_component_catalog("MyOS");
    REQUIRE(root != nullptr);
    CHECK_EQ(root->id, std::string("os"));
    CHECK_EQ(root->name, std::string("MyOS"));
    CHECK(root->kind == ComponentKind::Os);
    CHECK(root->parent == nullptr);
    REQUIRE(root->property("name") != nullptr);
    CHECK_EQ(root->property("name")->s, std::string("MyOS"));
    CHECK(root->property("name")->bound);
    CHECK(root->property("name")->type == PropertyType::String);
    CHECK(root->property("project_dir") != nullptr);
    CHECK(root->property("main_file") != nullptr);
    CHECK(root->property("declaration_status") != nullptr);
    REQUIRE(!root->sources.empty());
    CHECK(root->sources.front().base == SourceRef::Base::Project);
    CHECK_EQ(root->sources.front().path, std::string("main.cpp"));
}

TEST(catalog_every_id_is_unique_and_dotted_under_its_parent) {
    auto root = build_component_catalog("X");
    std::set<std::string> ids;
    walk(*root, [&](const Component& c) {
        CHECK(!c.id.empty());
        CHECK(!c.name.empty());
        CHECK(!c.summary.empty());
        CHECK(!c.description.empty());
        CHECK(ids.insert(c.id).second);
        if (c.parent && c.parent->id != "os") CHECK(c.id.starts_with(c.parent->id + "."));
        for (const auto& child : c.children) CHECK(child->parent == &c);
        std::set<std::string> props;
        for (const Property& p : c.properties) {
            CHECK(!p.id.empty());
            CHECK(!p.label.empty());
            CHECK(props.insert(p.id).second);
        }
    });
    CHECK(ids.size() >= 30u);
}

TEST(catalog_matches_the_architecture_table) {
    auto root = build_component_catalog("X");
    for (const Expected& e : kTable) {
        const Component* c = root->find(e.id);
        if (!c) {
            test::report_failure(__FILE__, __LINE__, std::string("missing component ") + e.id);
            continue;
        }
        CHECK_EQ(c->name, std::string(e.name));
        CHECK(c->kind == e.kind);
        CHECK(c->status == e.status);
    }
    // Programs and modules start empty: Project fills them from main.cpp.
    CHECK(root->find("user.programs")->children.empty());
    CHECK(root->find("user.modules")->children.empty());
}

TEST(catalog_bound_properties_exist) {
    auto root = build_component_catalog("X");
    struct Bound {
        const char* component;
        const char* property;
        PropertyType type;
    };
    const Bound bound[] = {
        {"os", "name", PropertyType::String},
        {"boot.firmware", "firmware", PropertyType::Enum},
        {"boot.firmware", "arch", PropertyType::Enum},
        {"boot.firmware", "boot_timeout", PropertyType::Int},
        {"hw.memory", "memory_mb", PropertyType::Int},
        {"hw.serial", "baud", PropertyType::Enum},
        {"hw.display", "resolution", PropertyType::Resolution},
    };
    for (const Bound& b : bound) {
        Component* c = root->find(b.component);
        REQUIRE(c != nullptr);
        const Property* p = c->property(b.property);
        if (!p) {
            test::report_failure(__FILE__, __LINE__, std::string("missing ") + b.component + "." + b.property);
            continue;
        }
        CHECK(p->bound);
        CHECK(p->type == b.type);
        CHECK(!p->read_only);
    }
    // Nothing else is bound.
    int count = 0;
    walk(*root, [&](const Component& c) {
        for (const Property& p : c.properties) count += p.bound ? 1 : 0;
    });
    CHECK_EQ(count, 7);

    const Property* fw = root->find("boot.firmware")->property("firmware");
    CHECK(fw->radio);
    REQUIRE_EQ(fw->options.size(), 2u);
    CHECK_EQ(fw->options[0].label, std::string("BIOS"));
    CHECK_EQ(fw->options[0].value, 1);
    CHECK_EQ(fw->options[1].label, std::string("UEFI"));
    CHECK_EQ(fw->options[1].value, 2);
    const Property* mem = root->find("hw.memory")->property("memory_mb");
    CHECK_EQ(mem->min, 32);
    CHECK_EQ(mem->max, 65536);
    CHECK_EQ(mem->step, 32);
    CHECK_EQ(mem->unit, std::string("MiB"));
    CHECK_EQ(mem->i, 128);
    const Property* timeout = root->find("boot.firmware")->property("boot_timeout");
    CHECK_EQ(timeout->min, 0);
    CHECK_EQ(timeout->max, 30);
    CHECK_EQ(timeout->unit, std::string("s"));
    const Property* baud = root->find("hw.serial")->property("baud");
    REQUIRE_EQ(baud->options.size(), 5u);
    CHECK_EQ(baud->options[0].value, 115200);
    CHECK_EQ(baud->options[4].value, 9600);
    for (const EnumOption& o : baud->options) CHECK_EQ(115200 % o.value, 0);
    const Property* layout = root->find("hw.keyboard")->property("layout");
    REQUIRE(layout != nullptr);
    CHECK(layout->read_only);
    CHECK(layout->type == PropertyType::Enum);
}

TEST(catalog_device_toggles) {
    auto root = build_component_catalog("X");
    std::set<std::string> toggles;
    walk(*root, [&](const Component& c) {
        if (c.toggleable) {
            toggles.insert(c.id);
            CHECK(!c.toggle_tooltip.empty());
        }
    });
    CHECK(toggles == (std::set<std::string>{"hw.serial", "hw.display", "hw.keyboard", "hw.rtc", "hw.pci", "hw.usb"}));
}

TEST(catalog_engine_sources_exist_on_disk) {
    auto root = build_component_catalog("X");
    const std::filesystem::path repo = test::repo_root();
    int engine_refs = 0;
    walk(*root, [&](const Component& c) {
        for (const SourceRef& s : c.sources) {
            CHECK(!s.path.empty());
            CHECK(!s.label.empty());
            if (s.base != SourceRef::Base::Engine) continue;
            ++engine_refs;
            if (!std::filesystem::is_regular_file(repo / s.path))
                test::report_failure(__FILE__, __LINE__, c.id + ": missing source " + s.path);
        }
        if (c.status == ComponentStatus::Planned) CHECK(c.sources.empty());
        if (c.status != ComponentStatus::Planned && c.kind != ComponentKind::Group) CHECK(!c.sources.empty());
    });
    CHECK(engine_refs >= 25);
}

TEST(catalog_table_sources_are_referenced) {
    auto root = build_component_catalog("X");
    auto has = [&](const char* id, const char* path) {
        const Component* c = root->find(id);
        if (!c) return false;
        for (const SourceRef& s : c->sources)
            if (s.path == path) return true;
        return false;
    };
    CHECK(has("boot.firmware", "engine/src/baremetal/boot.cpp"));
    CHECK(has("boot.firmware", "tools/osef-image/main.cpp"));
    CHECK(has("hw.cpu", "engine/src/baremetal/gdt.cpp"));
    CHECK(has("hw.cpu", "engine/src/baremetal/isr.S"));
    CHECK(has("hw.memory", "engine/src/baremetal/heap.cpp"));
    CHECK(has("hw.interrupts", "engine/src/baremetal/protect.cpp"));
    CHECK(has("hw.timer", "engine/src/baremetal/pit.cpp"));
    CHECK(has("hw.rtc", "engine/src/baremetal/rtc.cpp"));
    CHECK(has("hw.pci", "engine/src/baremetal/pci.cpp"));
    CHECK(has("hw.serial", "engine/src/baremetal/serial.cpp"));
    CHECK(has("hw.display", "engine/src/baremetal/fbconsole.cpp"));
    CHECK(has("hw.keyboard", "engine/src/baremetal/keyboard.cpp"));
    CHECK(has("hw.power", "engine/src/baremetal/panic.cpp"));
    CHECK(has("hw.power", "engine/src/baremetal/acpi.cpp"));
    CHECK(has("kernel.scheduler", "engine/src/scheduler.cpp"));
    CHECK(has("kernel.ipc", "engine/include/osef/ipc.h"));
    CHECK(has("kernel.vfs", "engine/src/vfs.cpp"));
    CHECK(has("user.shell", "engine/src/shell.cpp"));
    // The primary (first) source of a device is its driver.
    CHECK_EQ(root->find("hw.keyboard")->sources.front().path, std::string("engine/src/baremetal/keyboard.cpp"));
}

TEST(catalog_components_explain_support_and_next_steps) {
    auto root = build_component_catalog("X");
    struct Mention {
        const char* id;
        const char* text;
    };
    const Mention mentions[] = {{"hw.usb", "xHCI"},       {"hw.storage", "AHCI"}, {"hw.storage", "NVMe"},
                                {"hw.pci", "0xCF8"},     {"hw.mouse", "IRQ 12"}, {"hw.network", "virtio"},
                                {"hw.audio", "HDA"},     {"hw.rtc", "0x70"},      {"hw.power", "ACPI"}};
    for (const Mention& m : mentions) {
        const Component* c = root->find(m.id);
        REQUIRE(c != nullptr);
        if (c->description.find(m.text) == std::string::npos)
            test::report_failure(__FILE__, __LINE__, std::string(m.id) + " description lacks " + m.text);
        CHECK(!c->properties.empty());
    }
}

TEST(component_find_property_path_and_to_string) {
    auto root = build_component_catalog("MyOS");
    Component* kb = root->find("hw.keyboard");
    REQUIRE(kb != nullptr);
    CHECK_EQ(kb->path(), std::string("MyOS / Hardware / Keyboard"));
    CHECK_EQ(root->path(), std::string("MyOS"));
    CHECK(root->find("hw.nope") == nullptr);
    CHECK(kb->property("nope") == nullptr);
    CHECK(kb->property("irq") != nullptr);
    const Component& croot = *root;
    CHECK(croot.find("hw.keyboard") == kb);

    auto child = std::make_unique<Component>();
    child->id = "user.programs.demo";
    child->name = "demo";
    Component& added = root->find("user.programs")->add_child(std::move(child));
    CHECK(added.parent == root->find("user.programs"));
    CHECK(root->find("user.programs.demo") == &added);
    CHECK_EQ(added.path(), std::string("MyOS / Userland / Programs / demo"));

    CHECK_EQ(std::string(to_string(ComponentStatus::Planned)), std::string("Planned"));
    CHECK_EQ(std::string(to_string(ComponentStatus::Partial)), std::string("Partial"));
    CHECK_EQ(std::string(to_string(ComponentStatus::Implemented)), std::string("Implemented"));
    CHECK_EQ(std::string(to_string(ComponentKind::Usb)), std::string("USB"));
    CHECK_EQ(std::string(to_string(ComponentKind::Keyboard)), std::string("Keyboard"));
    for (int k = static_cast<int>(ComponentKind::Os); k <= static_cast<int>(ComponentKind::Module); ++k)
        CHECK(std::string(to_string(static_cast<ComponentKind>(k))) != "Component");
}
