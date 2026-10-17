// New OS wizard back end (model/new_project.h).

#include "test_main.h"

#include "model/hardware_decl.h"
#include "model/new_project.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST(new_project_validation) {
    NewProjectOptions o;
    o.folder = test::temp_dir("newproj_validate") / "os";
    CHECK(validate_new_project(o).empty());
    NewProjectOptions bad = o;
    bad.name = "";
    bad.firmware = "Coreboot";
    bad.screen = bad.serial = false;
    bad.memory_mb = 16;
    bad.look = "Plaid";
    CHECK_EQ(validate_new_project(bad).size(), size_t(5));
    bad = o;
    bad.name = "a \"quoted\" name";
    CHECK_EQ(validate_new_project(bad).size(), size_t(1));
    bad = o;
    bad.folder.clear();
    CHECK_EQ(validate_new_project(bad).size(), size_t(1));
}

TEST(new_project_main_declares_hardware) {
    NewProjectOptions o;
    o.name = "Tiny";
    o.firmware = "BIOS";
    o.keyboard = false;
    o.pci = true;
    o.memory_mb = 64;
    const std::string main = new_project_main(o);
    const HardwareParse p = parse_hardware_decl(main);
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.decl.name, std::string("Tiny"));
    CHECK_EQ(p.decl.firmware, std::string("BIOS"));
    CHECK(p.decl.serial && p.decl.framebuffer && !p.decl.ps2_keyboard && p.decl.rtc && p.decl.pci);
    CHECK_EQ(p.decl.memory_mb, 64u);
    CHECK(main.find("#include <osef/osef.h>") > main.find("OSEF_HARDWARE("));
    CHECK(main.find("int main()") != std::string::npos);
}

TEST(new_project_creates_files_once) {
    NewProjectOptions o;
    o.name = "Created";
    o.look = "Ocean";
    o.folder = test::temp_dir("newproj_create") / "nested" / "Created OS";
    std::string error;
    const fs::path main = create_new_project(o, &error);
    REQUIRE(!main.empty());
    CHECK(error.empty());
    CHECK(fs::exists(o.folder / kSettingsFile));
    CHECK(fs::exists(o.folder / kGeneratedFile));
    OsSettings s;
    REQUIRE(load_settings(o.folder / kSettingsFile, s, &error));
    CHECK(validate_settings(s).empty());
    CHECK_EQ(s.background, std::string("blue"));
    CHECK(current_look(s) && current_look(s)->name == "Ocean");
    CHECK(!s.programs.empty());
    // Never overwrites an existing OS.
    std::ofstream(main, std::ios::trunc) << "// mine\n";
    CHECK(create_new_project(o, &error).empty());
    CHECK(error.find("already contains") != std::string::npos);
    CHECK_EQ(read_file(main), std::string("// mine\n"));
}

TEST(look_presets_are_valid) {
    for (const LookPreset& p : look_presets()) {
        OsSettings s;
        apply_look(s, p);
        CHECK(validate_settings(s).empty());
        CHECK(current_look(s) == &p);
    }
    OsSettings custom;
    custom.text_color = "red";
    CHECK(current_look(custom) == nullptr);
    CHECK(default_project_folder("A/B").filename() == "A-B");
}
