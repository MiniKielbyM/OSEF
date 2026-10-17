#include "model/new_project.h"

#include "model/hardware_decl.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace osef::editor {

namespace fs = std::filesystem;

const std::vector<LookPreset>& look_presets() {
    static const std::vector<LookPreset> presets = {
        {"Classic", "Light gray text on black, like a terminal.", "default", "default", "bright-green", "bright-blue"},
        {"Ocean", "White text on deep blue.", "bright-white", "blue", "bright-yellow", "bright-cyan"},
        {"Matrix", "Green on black, hacker style.", "bright-green", "black", "green", "bright-green"},
        {"Sunset", "Yellow text on red.", "bright-yellow", "red", "bright-white", "yellow"},
        {"Paper", "Black text on white, easy to read.", "black", "bright-white", "blue", "magenta"},
    };
    return presets;
}

void apply_look(OsSettings& s, const LookPreset& look) {
    s.text_color = look.text_color;
    s.background = look.background;
    s.prompt_name_color = look.prompt_name_color;
    s.prompt_path_color = look.prompt_path_color;
}

const LookPreset* current_look(const OsSettings& s) {
    for (const LookPreset& p : look_presets())
        if (p.text_color == s.text_color && p.background == s.background &&
            p.prompt_name_color == s.prompt_name_color && p.prompt_path_color == s.prompt_path_color)
            return &p;
    return nullptr;
}

std::vector<std::string> validate_new_project(const NewProjectOptions& o) {
    std::vector<std::string> problems;
    if (o.name.empty()) problems.push_back("Give your OS a name.");
    if (o.name.size() > 31) problems.push_back("The name can be at most 31 characters long.");
    for (char c : o.name)
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            problems.push_back("The name cannot contain quotes, backslashes or control characters.");
            break;
        }
    if (o.firmware != "UEFI" && o.firmware != "BIOS") problems.push_back("Firmware must be UEFI or BIOS.");
    if (!o.screen && !o.serial) problems.push_back("Enable a console device: Framebuffer or COM1 UART.");
    if (o.memory_mb < 32) problems.push_back("memory_mb must be >= 32 (OSEF_HARDWARE static_assert).");
    if (o.folder.empty()) {
        problems.push_back("Choose a folder for the project.");
    } else {
        std::error_code ec;
        if (fs::exists(o.folder / "main.cpp", ec)) problems.push_back(o.folder.string() + " already contains an OS (main.cpp).");
        else if (fs::exists(o.folder, ec) && !fs::is_directory(o.folder, ec))
            problems.push_back(o.folder.string() + " is a file, not a folder.");
    }
    bool look_found = false;
    for (const LookPreset& p : look_presets()) look_found |= p.name == o.look;
    if (!look_found) problems.push_back("Unknown look '" + o.look + "'.");
    return problems;
}

std::string new_project_main(const NewProjectOptions& o) {
    HardwareDecl d;
    d.name = o.name;
    d.arch = "x86_64";
    d.firmware = o.firmware;
    d.serial = o.serial;
    d.framebuffer = o.screen;
    d.ps2_keyboard = o.keyboard;
    d.rtc = o.clock;
    d.pci = o.pci;
    d.memory_mb = o.memory_mb;
    return "// " + o.name +
           ", made with the OSEF editor.\n"
           "//\n"
           "// The hardware this OS runs on is declared below. Change it in the editor\n"
           "// (OS Settings or Inspector) or right here. Appearance, programs and files\n"
           "// live in osef-settings.ini and are compiled from osef_generated.cpp.\n\n"
           "#include <osef/hardware.h>\n\n" +
           generate_hardware_decl(d) +
           "\n\n"
           "#include <osef/osef.h>\n\n"
           "int main() {\n"
           "    osef::Kernel kernel;  // everything else comes from the OS settings\n"
           "    return kernel.run();\n"
           "}\n";
}

fs::path create_new_project(const NewProjectOptions& o, std::string* error) {
    const auto problems = validate_new_project(o);
    if (!problems.empty()) {
        if (error) *error = problems.front();
        return {};
    }
    std::error_code ec;
    fs::create_directories(o.folder, ec);
    if (ec) {
        if (error) *error = "cannot create " + o.folder.string() + ": " + ec.message();
        return {};
    }
    const fs::path main = o.folder / "main.cpp";
    {
        std::ofstream out(main, std::ios::binary | std::ios::trunc);
        if (!out || !(out << new_project_main(o))) {
            if (error) *error = "cannot write " + main.string();
            return {};
        }
    }
    OsSettings s;
    s.welcome = "Welcome to " + o.name + "! Type 'help' to see what you can do.";
    for (const LookPreset& p : look_presets())
        if (p.name == o.look) apply_look(s, p);
    if (o.sample_programs) {
        s.programs.push_back({"message", "hello", "says hello", "Hello from " + o.name + "!", 0});
        s.programs.push_back({"guess", "guess", "a number guessing game", "", 100});
        s.programs.push_back({"countdown", "countdown", "counts down from 5", "Lift off!", 5});
        if (o.clock) s.programs.push_back({"clock", "clock", "shows the date and time", "", 3});
    }
    s.files.push_back({"/home/readme.txt", "This file was created by the OSEF editor.\nTry: cat /home/readme.txt"});
    if (!write_settings_files(o.folder, s, error)) return {};
    return main;
}

fs::path default_project_folder(std::string_view name) {
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) home = std::getenv("USERPROFILE");
#endif
    std::string folder;
    for (char c : name) folder += (c == '/' || c == '\\' || c == ':') ? '-' : c;
    if (folder.empty()) folder = "MyOS";
    return fs::path(home ? home : ".") / "Documents" / "OSEF Projects" / folder;
}

} // namespace osef::editor
