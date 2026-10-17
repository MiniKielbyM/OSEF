// File browser: the OS project directory and the engine sources, listed
// lazily (a directory is read the first time it is expanded).

#include "panels/project_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace osef::editor {
namespace fs = std::filesystem;
namespace {

using theme::Color;

constexpr uintmax_t kMaxOpenBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxSearchResults = 500;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool skipped(const std::string& name, bool is_dir) {
    if (name.empty() || name[0] == '.') return true;  // hidden (.git, .toolchain, .DS_Store, ...)
    if (!is_dir) return false;
    static const char* const kSkipDirs[] = {"build", "out", "node_modules", "__pycache__", "third_party"};
    for (const char* d : kSkipDirs)
        if (name == d) return true;
    return name.rfind("cmake-build-", 0) == 0;
}

bool is_code_file(const fs::path& p) {
    static const char* const kExts[] = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
                                        ".s", ".S", ".asm", ".ld", ".py", ".sh", ".mk", ".cmake", ".json",
                                        ".toml", ".yml", ".yaml"};
    const std::string ext = p.extension().string();
    for (const char* e : kExts)
        if (ext == e) return true;
    const std::string name = p.filename().string();
    return name == "Makefile" || name == "CMakeLists.txt" || name == "GNUmakefile";
}

bool looks_binary(const fs::path& p) {
    static const char* const kExts[] = {".o", ".a", ".so", ".dylib", ".dll", ".exe", ".iso", ".img", ".bin",
                                        ".elf", ".png", ".jpg", ".jpeg", ".gif", ".ttf", ".otf", ".zip",
                                        ".gz", ".xz", ".tar", ".pdf", ".efi", ".sys"};
    const std::string ext = lower(p.extension().string());
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

// Single-quoted for /bin/sh.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

// Shows `path` in the platform file manager, detached. Failures are logged.
void reveal_in_file_manager(EditorContext& ctx, const fs::path& path) {
#if defined(_WIN32)
    const std::string cmd = "start \"\" explorer /select,\"" + path.string() + "\"";
#elif defined(__APPLE__)
    const std::string cmd = "open -R " + shell_quote(path.string()) + " >/dev/null 2>&1 &";
#else
    std::error_code ec;
    const fs::path dir = fs::is_directory(path, ec) ? path : path.parent_path();
    const std::string cmd = "xdg-open " + shell_quote(dir.string()) + " >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) ctx.log.warn("Could not reveal " + path.string());
}

const char* reveal_label() {
#if defined(_WIN32)
    return "Show in Explorer";
#elif defined(__APPLE__)
    return "Reveal in Finder";
#else
    return "Open Containing Folder";
#endif
}

struct Node {
    fs::path path;
    std::string name;
    bool is_dir = false;
    bool listed = false;
    std::string error;  // listing failure
    std::vector<std::unique_ptr<Node>> children;

    void list() {
        listed = true;
        children.clear();
        error.clear();
        std::error_code ec;
        fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            error = ec.message();
            return;
        }
        for (const fs::directory_entry& entry : it) {
            std::error_code e2;
            const bool dir = entry.is_directory(e2);
            const std::string n = entry.path().filename().string();
            if (skipped(n, dir)) continue;
            auto child = std::make_unique<Node>();
            child->path = entry.path();
            child->name = n;
            child->is_dir = dir;
            children.push_back(std::move(child));
        }
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
            if (a->is_dir != b->is_dir) return a->is_dir;
            return lower(a->name) < lower(b->name);
        });
    }
};

struct SearchHit {
    fs::path path;
    std::string relative;  // "<root label>/sub/dir"
};

} // namespace

struct ProjectPanel::State {
    fs::path project_dir;
    fs::path engine_dir;
    std::string project_label;
    std::unique_ptr<Node> roots[2];

    char filter[128] = {};
    std::string searched;  // filter the hits were computed for
    std::vector<SearchHit> hits;
    bool hits_truncated = false;

    fs::path selected;
    fs::path context_path;  // target of the open context menu
    bool context_is_dir = false;

    void reset(const EditorContext& ctx) {
        project_dir = ctx.project ? ctx.project->directory() : fs::path();
        engine_dir = ctx.engine_root.empty() ? fs::path() : ctx.engine_root / "engine";
        project_label = ctx.project ? ctx.project->name() : std::string("Project");
        for (auto& r : roots) r.reset();
        if (!project_dir.empty()) {
            roots[0] = std::make_unique<Node>();
            roots[0]->path = project_dir;
            roots[0]->name = project_label;
            roots[0]->is_dir = true;
        }
        if (!engine_dir.empty()) {
            roots[1] = std::make_unique<Node>();
            roots[1]->path = engine_dir;
            roots[1]->name = "Engine";
            roots[1]->is_dir = true;
        }
        searched.clear();
        hits.clear();
    }

    void search(const std::string& needle) {
        searched = needle;
        hits.clear();
        hits_truncated = false;
        const std::string n = lower(needle);
        for (const auto& root : roots) {
            if (!root) continue;
            std::error_code ec;
            fs::recursive_directory_iterator it(root->path, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::directory_entry& entry = *it;
                std::error_code e2;
                const bool dir = entry.is_directory(e2);
                const std::string name = entry.path().filename().string();
                if (skipped(name, dir) || it.depth() > 12) {
                    if (dir) it.disable_recursion_pending();
                    continue;
                }
                if (dir || lower(name).find(n) == std::string::npos) continue;
                if (hits.size() >= kMaxSearchResults) {
                    hits_truncated = true;
                    return;
                }
                SearchHit hit;
                hit.path = entry.path();
                hit.relative = root->name + "/" + entry.path().parent_path().lexically_relative(root->path).generic_string();
                if (!hit.relative.empty() && hit.relative.back() == '.') hit.relative.pop_back();
                if (!hit.relative.empty() && hit.relative.back() == '/') hit.relative.pop_back();
                hits.push_back(std::move(hit));
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const SearchHit& a, const SearchHit& b) { return a.path.filename() < b.path.filename(); });
    }
};

ProjectPanel::ProjectPanel() : state_(new State) {}
ProjectPanel::~ProjectPanel() { delete state_; }

namespace {

void open_in_editor(EditorContext& ctx, const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (looks_binary(path) || (!ec && size > kMaxOpenBytes)) {
        ctx.log.warn("Not opening " + path.filename().string() + ": binary or too large for the code editor");
        return;
    }
    ctx.open_file(path);
}

// Icon + label drawn after an unlabeled tree node / selectable.
void draw_icon_label(icons::Glyph glyph, ImU32 icon_color, const char* label, ImU32 text_color) {
    ImGui::SameLine(0.0f, 0.0f);
    const float size = std::floor(ImGui::GetFontSize());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = std::floor(p.y + (ImGui::GetTextLineHeight() - size) * 0.5f) + 1.0f;  // rows use 1 px frame padding
    icons::draw(ImGui::GetWindowDrawList(), ImVec2(p.x, y), size, glyph, icon_color);
    ImGui::Dummy(ImVec2(size + 4.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

} // namespace

void ProjectPanel::draw(EditorContext& ctx) {
    State& st = *state_;
    const fs::path project_dir = ctx.project ? ctx.project->directory() : fs::path();
    const fs::path engine_dir = ctx.engine_root.empty() ? fs::path() : ctx.engine_root / "engine";
    const std::string label = ctx.project ? ctx.project->name() : std::string("Project");
    if (project_dir != st.project_dir || engine_dir != st.engine_dir) st.reset(ctx);
    if (label != st.project_label && st.roots[0]) st.roots[0]->name = st.project_label = label;  // renamed in code

    // ---- Toolbar: filter + refresh ----
    const float refresh_w = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - refresh_w -
                                                ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", "Search files", st.filter, sizeof(st.filter));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        st.reset(ctx);
    }
    ImGui::SetItemTooltip("Re-read the directories from disk");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    const bool open = ImGui::BeginChild("##files", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::EndChild();
        return;
    }

    const ImU32 folder_color = IM_COL32(0xC8, 0xA8, 0x5A, 0xFF);
    const ImU32 file_color = theme::color(Color::TextDim);
    const ImU32 code_color = IM_COL32(0x7F, 0xA8, 0xE8, 0xFF);
    const ImU32 text = theme::color(Color::Text);

    // Context menu for whichever row was right-clicked.
    auto row_context = [&](const fs::path& path, bool is_dir) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            st.context_path = path;
            st.context_is_dir = is_dir;
            st.selected = path;
            ImGui::OpenPopup("##project_menu");
        }
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));
    if (st.filter[0]) {
        if (st.searched != st.filter) st.search(st.filter);
        if (st.hits.empty()) ImGui::TextDisabled("No files match \"%s\"", st.filter);
        for (const SearchHit& hit : st.hits) {
            ImGui::PushID(hit.path.string().c_str());
            const bool sel = st.selected == hit.path;
            if (ImGui::Selectable("##hit", sel, ImGuiSelectableFlags_AllowOverlap)) {
                st.selected = hit.path;
                open_in_editor(ctx, hit.path);
            }
            row_context(hit.path, false);
            const std::string name = hit.path.filename().string();
            draw_icon_label(is_code_file(hit.path) ? icons::Glyph::FileCode : icons::Glyph::File,
                            is_code_file(hit.path) ? code_color : file_color, name.c_str(), text);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", hit.relative.c_str());
            ImGui::PopID();
        }
        if (st.hits_truncated) ImGui::TextDisabled("(first %zu results)", kMaxSearchResults);
    } else {
        // Recursive tree. Directories are listed when first expanded.
        auto draw_node = [&](auto&& self, Node& node, bool is_root) -> void {
            if (is_root)
                ImGui::PushID(&node == st.roots[0].get() ? "project_root" : "engine_root");
            else
                ImGui::PushID(node.name.c_str());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_AllowOverlap |
                                       ImGuiTreeNodeFlags_FramePadding;
            if (!node.is_dir) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (st.selected == node.path) flags |= ImGuiTreeNodeFlags_Selected;
            if (is_root) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));
            const bool expanded = ImGui::TreeNodeEx("##node", flags);
            ImGui::PopStyleVar();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                st.selected = node.path;
                if (!node.is_dir) open_in_editor(ctx, node.path);
            }
            row_context(node.path, node.is_dir);
            if (is_root && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetItemTooltip("%s", node.path.string().c_str());

            icons::Glyph glyph = icons::Glyph::File;
            ImU32 color = file_color;
            if (node.is_dir) {
                glyph = expanded ? icons::Glyph::FolderOpen : icons::Glyph::Folder;
                color = folder_color;
            } else if (is_code_file(node.path)) {
                glyph = icons::Glyph::FileCode;
                color = code_color;
            }
            std::error_code ec;
            const bool missing = is_root && !fs::is_directory(node.path, ec);
            const std::string shown = missing ? node.name + " (not found)" : node.name;
            draw_icon_label(glyph, color, shown.c_str(), missing ? theme::color(Color::TextDim) : text);

            if (node.is_dir && expanded) {
                if (!node.listed) node.list();
                if (!node.error.empty()) ImGui::TextDisabled("(%s)", node.error.c_str());
                for (auto& child : node.children) self(self, *child, false);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        bool any = false;
        for (auto& root : st.roots) {
            if (!root) continue;
            any = true;
            draw_node(draw_node, *root, true);
        }
        if (!any) ImGui::TextDisabled("No project open");
    }
    ImGui::PopStyleVar();

    if (ImGui::BeginPopup("##project_menu")) {
        const fs::path& p = st.context_path;
        if (!st.context_is_dir && ImGui::MenuItem("Open")) open_in_editor(ctx, p);
        if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(p.string().c_str());
        if (ImGui::MenuItem(reveal_label())) reveal_in_file_manager(ctx, p);
        if (st.context_is_dir) {
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh")) st.reset(ctx);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

} // namespace osef::editor
