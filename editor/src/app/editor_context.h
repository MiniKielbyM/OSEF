#pragma once
// Shared editor state handed to every panel each frame.

#include "app/log.h"
#include "model/document.h"
#include "model/project.h"
#include "services/process_runner.h"

#include <filesystem>
#include <memory>
#include <string>

struct ImFont;

namespace osef::editor {

struct Fonts {
    ImFont* ui = nullptr;       // body text
    ImFont* ui_bold = nullptr;  // headings, selected names
    ImFont* mono = nullptr;     // code
    ImFont* small = nullptr;    // badges, hints
};

struct EditorContext {
    Log log;
    DocumentManager docs;
    std::unique_ptr<Project> project;
    ProcessRunner runner;
    Fonts fonts;
    std::filesystem::path engine_root;
    bool beginner_mode = true;

    std::string selection;  // selected component id, "" = none

    // Selects a component and opens its primary source in the code editor.
    void select(const std::string& component_id);
    Component* selected();  // nullptr when nothing (or something stale) is selected

    // Opens a file in the code editor, focuses its tab, reveals `line` (1-based) if > 0.
    Document* open_file(const std::filesystem::path& path, int line = 0);

    // Starts a build target in the background and focuses the Console.
    void run_target(const BuildTarget& target);

    // Panel -> app requests, consumed by EditorApp once per frame.
    bool request_focus_console = false;
    bool request_focus_code = false;
    bool request_focus_inspector = false;
};

} // namespace osef::editor
