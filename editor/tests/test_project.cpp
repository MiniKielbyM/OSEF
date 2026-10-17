// Project (model/project.h): opening an OS, code <-> Inspector sync, build
// targets and driver stubs. Works on temp copies, never on examples/.

#include "test_main.h"

#include "app/log.h"
#include "model/document.h"
#include "model/hardware_decl.h"
#include "model/project.h"
#include "text/text_buffer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// A project directory `name` under a fresh temp dir with `main_cpp` as main.cpp.
fs::path make_project(const std::string& tag, const std::string& name, const std::string& main_cpp) {
    fs::path dir = test::temp_dir(tag) / name;
    write_file(dir / "main.cpp", main_cpp);
    return dir;
}

const char* kNoDecl =
    "// Demo OS: no hardware declaration yet.\n"
    "\n"
    "#include <osef/osef.h>\n"
    "\n"
    "using namespace osef;\n"
    "\n"
    "class Blink final : public Module {\n"
    "public:\n"
    "    const char* name() const override { return \"blink\"; }\n"
    "};\n"
    "\n"
    "class Counter final : public Process {\n"
    "public:\n"
    "    void step(Context& c) override { c.exit(0); }\n"
    "};\n"
    "\n"
    "int main() {\n"
    "    Kernel kernel;\n"
    "    kernel.add_module<ShellModule>();\n"
    "    kernel.add_module<Blink>();\n"
    "    // kernel.add_module<Commented>();\n"
    "    auto& programs = kernel.programs();\n"
    "    programs.add<Counter>(\"count\", \"counts \\\"things\\\"\");\n"
    "    programs.add_fn(\"hello\", \"says hi\", [](Context& c) { c.exit(0); });\n"
    "    /* programs.add_fn(\"ghost\", \"commented out\", nullptr); */\n"
    "    const char* s = \"programs.add_fn(\\\"fake\\\", \\\"in a string\\\", f)\";\n"
    "    return kernel.run();\n"
    "}\n";

struct Opened {
    Log log;
    DocumentManager docs;
    std::unique_ptr<Project> project;
};

std::unique_ptr<Opened> open_project(const fs::path& location) {
    auto o = std::make_unique<Opened>();
    std::string error;
    o->project = Project::open(location, test::repo_root(), o->docs, o->log, &error);
    if (!o->project) test::report_failure(__FILE__, __LINE__, "Project::open failed: " + error);
    return o;
}

const Component& child(const Component& c, size_t i) { return *c.children.at(i); }

} // namespace

TEST(project_opens_a_copy_of_myos) {
    fs::path dir = test::temp_dir("project_myos") / "myos";
    fs::copy(test::repo_root() / "examples" / "myos", dir, fs::copy_options::recursive);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    CHECK(p.hardware().found);
    CHECK(p.hardware().problems.empty());
    CHECK_EQ(p.name(), p.hardware().decl.name);
    CHECK_EQ(p.root().name, p.name());
    CHECK(fs::equivalent(p.directory(), dir));
    CHECK(fs::equivalent(p.main_file(), dir / "main.cpp"));
    CHECK(fs::equivalent(p.engine_root(), test::repo_root()));
    REQUIRE(p.main_document() != nullptr);
    CHECK(o->docs.find(dir / "main.cpp") == p.main_document());
    CHECK(!p.find("user.programs")->children.empty());
    CHECK(!p.find("user.modules")->children.empty());
    CHECK(p.find("user.modules.ShellModule") != nullptr);
    const std::string status = p.root().property("declaration_status")->s;
    CHECK(status.starts_with("Declared at main.cpp:"));
    CHECK(o->log.size() >= 1u);

    // Opening the .cpp directly is equivalent.
    auto o2 = open_project(dir / "main.cpp");
    REQUIRE(o2->project);
    CHECK(fs::equivalent(o2->project->directory(), dir));
}

TEST(project_open_errors) {
    Log log;
    DocumentManager docs;
    std::string error;
    fs::path tmp = test::temp_dir("project_errors");
    CHECK(Project::open(tmp / "nope", test::repo_root(), docs, log, &error) == nullptr);
    CHECK(!error.empty());
    error.clear();
    CHECK(Project::open(tmp, test::repo_root(), docs, log, &error) == nullptr);  // no main.cpp
    CHECK(error.find("main.cpp") != std::string::npos);
    fs::path dir = make_project("project_errors_root", "os", kNoDecl);
    error.clear();
    CHECK(Project::open(dir, tmp, docs, log, &error) == nullptr);  // not an OSEF checkout
    CHECK(error.find("hardware.h") != std::string::npos);
    write_file(tmp / "notes.txt", "x");
    error.clear();
    CHECK(Project::open(tmp / "notes.txt", test::repo_root(), docs, log, &error) == nullptr);
    CHECK(!error.empty());
}

TEST(project_add_declaration_toggle_and_firmware) {
    fs::path dir = make_project("project_edit", "demo", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    Document& doc = *p.main_document();

    CHECK(!p.hardware().found);
    CHECK_EQ(p.name(), std::string("demo"));
    const std::string missing = p.root().property("declaration_status")->s;
    CHECK(missing.starts_with("Missing"));
    // Without a declaration the Inspector shows what one would get.
    CHECK_EQ(p.find("boot.firmware")->property("firmware")->i, 2);
    CHECK(p.find("hw.keyboard")->enabled);

    p.add_hardware_declaration();
    REQUIRE(p.hardware().found);
    CHECK(p.hardware().problems.empty());
    HardwareDecl expect;
    expect.name = "demo";
    expect.arch = "x86_64";
    expect.firmware = "UEFI";
    expect.serial = expect.framebuffer = expect.ps2_keyboard = true;
    expect.memory_mb = 128;
    CHECK(p.hardware().decl == expect);
    std::string text = doc.text();
    CHECK(text.starts_with("// Demo OS: no hardware declaration yet.\n\n#include <osef/hardware.h>\n\n"
                           "OSEF_HARDWARE(osef::Hardware()\n    .name(\"demo\")\n"));
    CHECK(text.find("    .memory_mb(128));\n\n#include <osef/osef.h>\n") != std::string::npos);
    CHECK_EQ(p.hardware().line, 5);
    CHECK_EQ(p.root().property("declaration_status")->s, std::string("Declared at main.cpp:5"));
    CHECK(doc.modified());
    CHECK_EQ(read_file(dir / "main.cpp"), std::string(kNoDecl));  // nothing saved

    // Adding again is a no-op.
    uint64_t v = doc.version();
    p.add_hardware_declaration();
    CHECK_EQ(doc.version(), v);

    // Toggle the keyboard off.
    Component* kb = p.find("hw.keyboard");
    REQUIRE(kb != nullptr);
    kb->enabled = false;
    p.apply_enabled(*kb);
    text = doc.text();
    CHECK(text.find(".devices(osef::Hw::Serial | osef::Hw::Framebuffer)\n") != std::string::npos);
    CHECK(!parse_hardware_decl(text).decl.ps2_keyboard);
    CHECK(!p.find("hw.keyboard")->enabled);

    for (const char* id : {"hw.rtc", "hw.pci"}) {
        Component* device = p.find(id);
        REQUIRE(device != nullptr);
        device->enabled = true;
        p.apply_enabled(*device);
    }
    CHECK(parse_hardware_decl(doc.text()).decl.rtc);
    CHECK(parse_hardware_decl(doc.text()).decl.pci);

    // Firmware -> BIOS through the Inspector property.
    Component* fw = p.find("boot.firmware");
    Property* firmware = fw->property("firmware");
    firmware->i = 1;
    p.apply_property(*fw, *firmware);
    text = doc.text();
    CHECK(text.find("    .firmware(osef::Firmware::BIOS)\n") != std::string::npos);
    HardwareParse parsed = parse_hardware_decl(text);
    CHECK_EQ(parsed.decl.firmware, std::string("BIOS"));
    CHECK(!parsed.decl.ps2_keyboard);
    CHECK_EQ(p.find("boot.firmware")->property("firmware")->i, 1);
    CHECK(p.find("boot.firmware")->property("image")->s.find("BIOS") != std::string::npos);
    // Everything outside the declaration is untouched.
    CHECK(text.ends_with(std::string(kNoDecl).substr(std::string(kNoDecl).find("\n#include <osef/osef.h>"))));

    // Each Inspector edit is one undo step.
    REQUIRE(doc.buffer().undo());
    p.sync_from_code();
    CHECK_EQ(p.hardware().decl.firmware, std::string("UEFI"));
    CHECK_EQ(p.find("boot.firmware")->property("firmware")->i, 2);
    CHECK(!p.hardware().decl.ps2_keyboard);
    REQUIRE(doc.buffer().redo());
    p.sync_from_code();
    CHECK_EQ(p.hardware().decl.firmware, std::string("BIOS"));
}

TEST(project_more_bound_properties) {
    fs::path dir = make_project("project_props", "props", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    p.add_hardware_declaration();

    Component* mem = p.find("hw.memory");
    mem->property("memory_mb")->i = 512;
    p.apply_property(*mem, *mem->property("memory_mb"));
    CHECK_EQ(p.hardware().decl.memory_mb, 512u);

    Component* serial = p.find("hw.serial");
    serial->property("baud")->i = 9600;
    p.apply_property(*serial, *serial->property("baud"));
    CHECK_EQ(p.hardware().decl.serial_baud, 9600u);
    CHECK(p.main_document()->text().find(".serial_baud(9600)") != std::string::npos);
    // An invalid rate is rejected and the property snaps back.
    serial->property("baud")->i = 1000;
    p.apply_property(*serial, *serial->property("baud"));
    CHECK_EQ(p.hardware().decl.serial_baud, 9600u);
    CHECK_EQ(p.find("hw.serial")->property("baud")->i, 9600);

    Component* display = p.find("hw.display");
    display->property("resolution")->i = 1280;
    display->property("resolution")->i2 = 720;
    p.apply_property(*display, *display->property("resolution"));
    CHECK(p.main_document()->text().find(".resolution(1280, 720)") != std::string::npos);

    Component* fw = p.find("boot.firmware");
    fw->property("boot_timeout")->i = 99;  // clamped to the property's 0-30
    p.apply_property(*fw, *fw->property("boot_timeout"));
    CHECK_EQ(p.hardware().decl.boot_timeout_s, 30u);

    p.root().property("name")->s = "Renamed \"OS\"";
    p.apply_property(p.root(), *p.root().property("name"));
    CHECK_EQ(p.hardware().decl.name, std::string("Renamed \"OS\""));
    CHECK_EQ(p.name(), std::string("Renamed \"OS\""));
    CHECK_EQ(p.root().name, p.name());
    CHECK(p.main_document()->text().find(".name(\"Renamed \\\"OS\\\"\")") != std::string::npos);

    // Unbound properties do nothing.
    uint64_t v = p.main_document()->version();
    Component* kb = p.find("hw.keyboard");
    p.apply_property(*kb, *kb->property("irq"));
    CHECK_EQ(p.main_document()->version(), v);
    CHECK(p.hardware().problems.empty());
}

TEST(project_code_edits_update_the_model) {
    fs::path dir = make_project("project_code", "code", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    p.add_hardware_declaration();
    Document& doc = *p.main_document();

    std::string text = doc.text();
    size_t at = text.find(".memory_mb(128)");
    REQUIRE(at != std::string::npos);
    text.replace(at, 15, ".memory_mb(0x400)");
    doc.replace_all(text);
    p.sync_from_code();
    CHECK_EQ(p.find("hw.memory")->property("memory_mb")->i, 1024);

    // Removing a console device and the firmware surfaces the compiler's messages.
    text = doc.text();
    at = text.find("    .firmware(osef::Firmware::UEFI)\n");
    REQUIRE(at != std::string::npos);
    text.erase(at, 36);
    doc.replace_all(text);
    p.sync_from_code();
    CHECK_EQ(p.find("boot.firmware")->property("firmware")->i, 0);
    const std::string status = p.root().property("declaration_status")->s;
    CHECK(status.find("1 problem") != std::string::npos);
    CHECK(status.find("choose the boot firmware") != std::string::npos);

    // Code the parser cannot read is never overwritten by Inspector edits.
    text = doc.text();
    at = text.find(".memory_mb(0x400)");
    text.replace(at, 17, ".memory_mb(kMemory)");
    doc.replace_all(text);
    p.sync_from_code();
    const std::string before = doc.text();
    Component* kb = p.find("hw.keyboard");
    kb->enabled = false;
    p.apply_enabled(*kb);
    CHECK_EQ(doc.text(), before);
    CHECK(p.find("hw.keyboard")->enabled);  // snapped back to the code
    CHECK(o->log.count(LogLevel::Warning) >= 1u);

    // Deleting the declaration: status says so, programs still discovered.
    HardwareParse hp = parse_hardware_decl(before);
    doc.replace_all(before.substr(0, hp.begin) + before.substr(hp.end));
    p.sync_from_code();
    CHECK(!p.hardware().found);
    CHECK(p.root().property("declaration_status")->s.starts_with("Missing"));
    CHECK_EQ(p.name(), std::string("code"));
}

TEST(project_discovers_programs_and_modules) {
    fs::path dir = make_project("project_scan", "scan", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    const Component* programs = p.find("user.programs");
    const Component* modules = p.find("user.modules");
    REQUIRE(programs != nullptr);
    REQUIRE(modules != nullptr);
    REQUIRE_EQ(programs->children.size(), 2u);
    REQUIRE_EQ(modules->children.size(), 2u);

    const Component& count = child(*programs, 0);
    CHECK_EQ(count.id, std::string("user.programs.count"));
    CHECK_EQ(count.name, std::string("count"));
    CHECK(count.kind == ComponentKind::Program);
    CHECK_EQ(count.summary, std::string("counts \"things\""));
    CHECK_EQ(count.property("kind")->s, std::string("class Counter"));
    CHECK_EQ(count.property("description")->s, std::string("counts \"things\""));
    REQUIRE(count.sources.size() >= 2u);
    CHECK(count.sources[0].base == SourceRef::Base::Project);
    CHECK_EQ(count.sources[0].path, std::string("main.cpp"));
    CHECK_EQ(count.sources[0].line, 23);
    CHECK_EQ(count.sources[1].line, 12);  // class Counter
    CHECK(count.parent == programs);

    const Component& hello = child(*programs, 1);
    CHECK_EQ(hello.id, std::string("user.programs.hello"));
    CHECK_EQ(hello.property("kind")->s, std::string("lambda (add_fn)"));
    CHECK_EQ(hello.sources[0].line, 24);

    CHECK_EQ(child(*modules, 0).id, std::string("user.modules.ShellModule"));
    CHECK_EQ(child(*modules, 0).sources[0].line, 19);
    CHECK_EQ(child(*modules, 1).id, std::string("user.modules.Blink"));
    CHECK_EQ(child(*modules, 1).property("type")->s, std::string("Blink"));
    CHECK_EQ(child(*modules, 1).sources[1].line, 7);
    CHECK_EQ(programs->property("count")->s, std::string("2"));
    CHECK_EQ(modules->property("count")->s, std::string("2"));

    // Editing code updates the children; unchanged ids keep their objects.
    Document& doc = *p.main_document();
    std::string text = doc.text();
    size_t at = text.find("    return kernel.run();");
    text.insert(at, "    kernel.programs().add<osef::Counter>(\"tally\", \"another\");\n"
                    "    kernel.add_module<drivers::UsbModule>();\n");
    doc.replace_all(text);
    p.sync_from_code();
    REQUIRE_EQ(programs->children.size(), 3u);
    CHECK_EQ(child(*programs, 2).id, std::string("user.programs.tally"));
    CHECK_EQ(child(*programs, 2).property("kind")->s, std::string("class osef::Counter"));
    REQUIRE_EQ(modules->children.size(), 3u);
    CHECK_EQ(child(*modules, 2).id, std::string("user.modules.UsbModule"));
    CHECK_EQ(child(*modules, 2).property("type")->s, std::string("drivers::UsbModule"));

    const Component* hello_before = &child(*programs, 1);
    text = doc.text();
    at = text.find("    programs.add<Counter>");
    text.insert(at, "    // a comment line shifts registrations down\n");
    doc.replace_all(text);
    p.sync_from_code();
    REQUIRE_EQ(programs->children.size(), 3u);
    CHECK(&child(*programs, 1) == hello_before);
    CHECK_EQ(child(*programs, 1).sources[0].line, 25);
}

TEST(project_resolve_and_build_targets) {
    fs::path dir = make_project("project_targets", "it's my os", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    SourceRef engine_ref{SourceRef::Base::Engine, "engine/src/baremetal/keyboard.cpp", "kb", 0};
    CHECK(fs::equivalent(p.resolve(engine_ref), test::repo_root() / "engine/src/baremetal/keyboard.cpp"));
    SourceRef project_ref{SourceRef::Base::Project, "main.cpp", "main", 0};
    CHECK(fs::equivalent(p.resolve(project_ref), dir / "main.cpp"));
    for (const char* id : {"hw.keyboard", "boot.firmware", "kernel.vfs", "user.shell"})
        for (const SourceRef& s : p.find(id)->sources) CHECK(fs::exists(p.resolve(s)));

    std::vector<BuildTarget> targets = p.build_targets();
    REQUIRE_EQ(targets.size(), 4u);
    const char* ids[] = {"run", "iso", "qemu", "test"};
    for (size_t i = 0; i < 4; ++i) {
        CHECK_EQ(targets[i].id, std::string(ids[i]));
        CHECK(!targets[i].label.empty());
        CHECK(!targets[i].tooltip.empty());
        CHECK(fs::equivalent(targets[i].cwd, test::repo_root()));
    }
#if !defined(_WIN32)
    const std::string quoted = "'" + p.directory().parent_path().string() + "/it'\\''s my os'";
    CHECK_EQ(targets[0].command, "make run OS_DIR=" + quoted);
    CHECK_EQ(targets[1].command, "make iso OS_DIR=" + quoted);
    CHECK_EQ(targets[2].command, "make run-iso OS_DIR=" + quoted);
#endif
    CHECK_EQ(targets[3].command, std::string("make test"));
    CHECK_EQ(targets[0].label, std::string("Run (hosted)"));
    CHECK_EQ(targets[1].label, std::string("Build ISO"));
    CHECK_EQ(targets[2].label, std::string("Run in QEMU"));
    CHECK_EQ(targets[3].label, std::string("Run Tests"));
}

TEST(project_driver_stub_written_once) {
    fs::path dir = make_project("project_stub", "stubos", kNoDecl);
    auto o = open_project(dir);
    REQUIRE(o->project);
    Project& p = *o->project;
    std::string error;
    fs::path path = p.create_driver_stub(*p.find("hw.mouse"), &error);
    REQUIRE(!path.empty());
    CHECK(error.empty());
    CHECK(fs::equivalent(path, dir / "drivers" / "mouse.cpp"));
    const std::string text = read_file(path);
    CHECK(text.find("#include <osef/osef.h>") != std::string::npos);
    CHECK(text.find("class MouseModule final : public osef::Module") != std::string::npos);
    CHECK(text.find("const char* name() const override { return \"mouse\"; }") != std::string::npos);
    CHECK(text.find("kernel.log(osef::LogLevel::Info, \"mouse: ") != std::string::npos);
    CHECK(text.find("kernel.register_device(\"mouse0\", {\"Mouse (stub)\", nullptr, nullptr});") != std::string::npos);
    CHECK(text.find("void on_tick(osef::Kernel&") != std::string::npos);
    CHECK(text.find("kernel.add_module<drivers::MouseModule>()") != std::string::npos);
    CHECK(text.find("PS/2") != std::string::npos);  // from the component's description
    CHECK(text.find("std::") == std::string::npos);
    // Every comment line fits in 80 columns.
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (text.compare(start, 2, "//") == 0) CHECK(end - start <= 80);
        start = end + 1;
    }

    // Never overwritten.
    write_file(path, "// my edits\n");
    error.clear();
    CHECK(p.create_driver_stub(*p.find("hw.mouse"), &error).empty());
    CHECK(error.find("already exists") != std::string::npos);
    CHECK_EQ(read_file(path), std::string("// my edits\n"));

    // Other planned components get their own file; implemented ones are refused.
    error.clear();
    fs::path storage = p.create_driver_stub(*p.find("hw.storage"), &error);
    REQUIRE(!storage.empty());
    CHECK(fs::equivalent(storage, dir / "drivers" / "storage.cpp"));
    CHECK(read_file(storage).find("class StorageModule") != std::string::npos);
    // PCI is implemented in the engine now, so it gets no stub.
    error.clear();
    CHECK(p.create_driver_stub(*p.find("hw.pci"), &error).empty());
    CHECK(!error.empty());
    error.clear();
    CHECK(p.create_driver_stub(*p.find("hw.keyboard"), &error).empty());
    CHECK(!error.empty());
    error.clear();
    CHECK(p.create_driver_stub(*p.find("hw"), &error).empty());
    CHECK(!error.empty());
    CHECK(!fs::exists(dir / "drivers" / "keyboard.cpp"));
}
