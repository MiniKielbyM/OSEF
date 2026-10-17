#pragma once
// "New OS" wizard back end: creates a ready-to-build OS project folder with a
// main.cpp (hardware declaration + a minimal main) and its OS settings.

#include "model/os_settings.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

// A named look for the OS console ("Ocean", "Matrix", ...).
struct LookPreset {
    std::string name;
    std::string description;
    std::string text_color, background, prompt_name_color, prompt_path_color;
};
const std::vector<LookPreset>& look_presets();
void apply_look(OsSettings& settings, const LookPreset& look);
// The preset whose colors match `settings`, or nullptr ("Custom").
const LookPreset* current_look(const OsSettings& settings);

struct NewProjectOptions {
    std::string name = "MyOS";             // OS name (1-31 characters)
    std::filesystem::path folder;          // created if missing; must not contain a main.cpp yet
    std::string firmware = "UEFI";         // "UEFI" (modern PCs) or "BIOS" (older PCs)
    bool screen = true;                    // osef::Hw::Framebuffer
    bool keyboard = true;                  // osef::Hw::PS2Keyboard
    bool serial = true;                    // osef::Hw::Serial
    bool clock = true;                     // osef::Hw::RTC
    bool pci = false;                      // osef::Hw::PCI
    bool usb = false;                      // osef::Hw::USB
    unsigned memory_mb = 128;
    std::string look = "Classic";          // a look_presets() name
    bool sample_programs = true;           // add a few template programs to try
};

// Problems that stop project creation (empty when the options are fine).
std::vector<std::string> validate_new_project(const NewProjectOptions& options);

// The main.cpp text for these options.
std::string new_project_main(const NewProjectOptions& options);

// Creates the folder, main.cpp, osef-settings.ini and osef_generated.cpp.
// Returns the path of main.cpp, or empty + *error. Never overwrites a main.cpp.
std::filesystem::path create_new_project(const NewProjectOptions& options, std::string* error);

// A suggested folder for a new OS: ~/Documents/OSEF Projects/<name>.
std::filesystem::path default_project_folder(std::string_view name);

} // namespace osef::editor
