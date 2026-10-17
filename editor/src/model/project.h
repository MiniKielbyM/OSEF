#pragma once
// An open OS project: its main.cpp, the component hierarchy built from the
// catalog plus what the code declares, and the code <-> inspector bindings.

#include "model/component.h"
#include "model/hardware_decl.h"
#include "model/os_settings.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

class Document;
class DocumentManager;
class Log;

struct BuildTarget {
    std::string id;                 // "run", "iso", "qemu", "test"
    std::string label;              // "Run (hosted)"
    std::string tooltip;
    std::string command;            // shell command line
    std::filesystem::path cwd;
};

class Project {
public:
    // `location`: an OS directory containing main.cpp, or a .cpp file.
    // `engine_root`: the OSEF checkout (directory containing engine/include/osef).
    // Opens main.cpp through `docs` (it stays open for the project's lifetime).
    static std::unique_ptr<Project> open(const std::filesystem::path& location,
                                         const std::filesystem::path& engine_root,
                                         DocumentManager& docs, Log& log, std::string* error);
    ~Project();
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    const std::string& name() const;                   // declared name, else the directory name
    const std::filesystem::path& directory() const;    // the OS project directory
    const std::filesystem::path& main_file() const;
    const std::filesystem::path& engine_root() const;
    Document* main_document() const;

    Component& root();                                 // kind Os
    Component* find(std::string_view component_id);

    const HardwareParse& hardware() const;             // latest parse of main.cpp

    // Code -> model. Call once per frame: re-parses main.cpp only when its
    // document version changed, then refreshes bound properties, enabled flags
    // and the code-derived children (Programs, Modules).
    void sync_from_code();

    // Model -> code. Call after the Inspector edits a property or toggles a
    // component. Rewrites the OSEF_HARDWARE declaration in the main.cpp
    // document (one undoable edit, not saved to disk).
    void apply_property(Component& component, const Property& property);
    void apply_enabled(Component& component);
    void add_hardware_declaration();                   // inserts a default one if missing

    // Plain-language settings from <project>/osef-settings.ini (defaults when
    // the file does not exist yet). Edit settings(), then call settings_changed()
    // to write osef-settings.ini and the generated osef_generated.cpp.
    OsSettings& settings();
    bool settings_changed(std::string* error = nullptr);
    std::vector<std::string> settings_problems() const;   // validate_settings()
    bool uses_settings() const;                          // osef-settings.ini exists

    std::filesystem::path resolve(const SourceRef& ref) const;   // absolute path
    std::vector<BuildTarget> build_targets() const;

    // Writes a starter driver for a Planned component to
    // <project>/drivers/<id>.cpp and returns its path (empty + *error on failure).
    // Never overwrites an existing file.
    std::filesystem::path create_driver_stub(const Component& component, std::string* error);

private:
    Project();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osef::editor
