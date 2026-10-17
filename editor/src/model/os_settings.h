#pragma once
// Plain-language OS settings edited by forms in the editor (no code needed):
// appearance, features, simple programs made from templates, and files that
// exist when the OS starts.
//
// They are stored in <project>/osef-settings.ini and compiled into the OS
// through a generated <project>/osef_generated.cpp. That file defines one
// OSEF_AUTO_MODULE, so main.cpp needs no extra line: the build compiles every
// .cpp in the project directory.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

constexpr const char* kSettingsFile = "osef-settings.ini";
constexpr const char* kGeneratedFile = "osef_generated.cpp";

// A program built from a template. Which fields matter depends on `kind`.
struct ProgramSpec {
    std::string kind = "message";  // see program_templates()
    std::string name;              // the command the user types, e.g. "hello"
    std::string description;       // shown by `help`
    std::string text;              // message / question / final text
    int number = 10;               // seconds, highest number, repetitions

    bool operator==(const ProgramSpec&) const = default;
};

// A file that exists when the OS starts (in its in-memory file system).
struct FileSpec {
    std::string path;              // absolute, e.g. "/etc/motd" or "/home/readme.txt"
    std::string content;

    bool operator==(const FileSpec&) const = default;
};

struct OsSettings {
    // ---- Appearance ----
    std::string welcome;                         // shown when the shell starts
    std::string prompt = "{os}:{cwd}$ ";         // {os} = OS name, {cwd} = current folder
    std::string text_color = "default";          // color names: see color_names()
    std::string background = "default";
    std::string prompt_name_color = "bright-green";
    std::string prompt_path_color = "bright-blue";
    int text_size = 0;                           // 0 = automatic, 1-4 = font scale

    // ---- Features ----
    bool shell = true;                           // include the command shell
    std::string start_program = "sh";            // what runs at boot: "sh" or one of `programs`

    std::vector<ProgramSpec> programs;
    std::vector<FileSpec> files;

    bool operator==(const OsSettings&) const = default;
};

// Template catalog for the "Add program" form.
struct ProgramTemplate {
    std::string kind;
    std::string label;          // "Show a message"
    std::string description;    // what the program does, in plain words
    std::string text_label;     // label for ProgramSpec::text, "" = unused
    std::string text_default;
    std::string number_label;   // label for ProgramSpec::number, "" = unused
    int number_default = 0;
    int number_min = 0;
    int number_max = 0;
};
const std::vector<ProgramTemplate>& program_templates();
const ProgramTemplate* find_program_template(std::string_view kind);

// The 16 console color names plus "default", in ConsoleColor order.
const std::vector<std::string>& color_names();
// "bright-green" -> "BrightGreen" (the osef::ConsoleColor enumerator), "" if unknown.
std::string color_enum_name(std::string_view color);

// Plain-language problems that would make the OS fail to build or misbehave
// (bad command names, duplicates, a start program that does not exist, ...).
std::vector<std::string> validate_settings(const OsSettings& settings);
// A valid command name: lowercase letters, digits, '-' or '_', starting with a letter.
bool is_valid_command_name(std::string_view name);
// Command names the OS already has (shell builtins), which programs must not reuse.
const std::vector<std::string>& reserved_command_names();

// ---- Storage ----
std::string serialize_settings(const OsSettings& settings);
// Unknown keys/sections are ignored (and reported in *warnings).
OsSettings parse_settings(std::string_view text, std::vector<std::string>* warnings = nullptr);
bool load_settings(const std::filesystem::path& file, OsSettings& out, std::string* error);
bool save_settings(const std::filesystem::path& file, const OsSettings& settings, std::string* error);

// ---- Code generation ----
// The complete osef_generated.cpp for these settings.
std::string generate_settings_code(const OsSettings& settings);

// Writes osef-settings.ini and osef_generated.cpp into `project_dir`
// (atomically, only when their content changed). Returns false + *error.
bool write_settings_files(const std::filesystem::path& project_dir, const OsSettings& settings,
                          std::string* error);

} // namespace osef::editor
