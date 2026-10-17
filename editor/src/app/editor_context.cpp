// EditorContext helpers shared by the panels. Errors go to the Console log.

#include "app/editor_context.h"

#include "model/component.h"

#include <system_error>

namespace osef::editor {
namespace {

// Opens `path` as the active document and asks the code view to show `line`.
Document* open_document(EditorContext& ctx, const std::filesystem::path& path, int line) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        ctx.log.error("Cannot open " + path.string() + ": file not found");
        return nullptr;
    }
    std::string error;
    Document* doc = ctx.docs.open(path, &error);
    if (!doc) {
        ctx.log.error("Cannot open " + path.string() + (error.empty() ? std::string() : ": " + error));
        return nullptr;
    }
    ctx.docs.set_active(doc);
    if (line > 0) doc->request_reveal_line(line);
    return doc;
}

} // namespace

void EditorContext::select(const std::string& component_id) {
    if (component_id.empty() || !project) {
        selection.clear();
        return;
    }
    Component* component = project->find(component_id);
    if (!component) {
        log.warn("Unknown component '" + component_id + "'");
        return;
    }
    selection = component_id;
    // Show its primary source without stealing keyboard focus from the
    // Hierarchy (arrow-key navigation keeps working).
    if (!component->sources.empty()) {
        const SourceRef& ref = component->sources.front();
        open_document(*this, project->resolve(ref), ref.line);
    }
}

Component* EditorContext::selected() {
    if (selection.empty() || !project) return nullptr;
    return project->find(selection);  // nullptr when the id went stale (e.g. a renamed program)
}

Document* EditorContext::open_file(const std::filesystem::path& path, int line) {
    Document* doc = open_document(*this, path, line);
    if (doc) request_focus_code = true;
    return doc;
}

void EditorContext::run_target(const BuildTarget& target) {
    if (runner.running()) {
        log.warn("Cannot start " + target.label + ": " + runner.current_label() + " is still running");
        request_focus_console = true;
        return;
    }
    // Builds read files from disk, so save first (like most IDEs do).
    if (docs.any_modified()) {
        int count = 0;
        for (const auto& doc : docs.documents())
            if (doc->modified()) ++count;
        std::string error;
        if (!docs.save_all(&error)) {
            log.error("Not starting " + target.label + ": saving failed" + (error.empty() ? "" : ": " + error));
            request_focus_console = true;
            return;
        }
        log.info("Saved " + std::to_string(count) + (count == 1 ? " file" : " files") + " before " + target.label);
    }
    runner.start(target.label, target.command, target.cwd, log);
    request_focus_console = true;
}

} // namespace osef::editor
