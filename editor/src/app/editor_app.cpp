// EditorApp: menu bar, toolbar, dockspace + default layout, status bar,
// global shortcuts and dialogs around the panels.

#include "app/editor_app.h"

#include "app/icons.h"
#include "app/theme.h"
#include "model/component.h"
#include "model/document.h"
#include "model/project.h"
#include "panels/code_panel.h"
#include "panels/console_panel.h"
#include "panels/hierarchy_panel.h"
#include "panels/inspector_panel.h"
#include "panels/project_panel.h"
#include "panels/settings_panel.h"
#include "model/new_project.h"
#include "text/text_buffer.h"

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder, settings handlers, error recovery, sidebars
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace osef::editor {
namespace fs = std::filesystem;
namespace {

using theme::Color;

constexpr const char* kDockspaceName = "OSEFDockSpace";
constexpr const char* kSettingsType = "OSEFPanels";

enum EditCommand { kUndo, kRedo, kCut, kCopy, kPaste, kSelectAll, kFind };

bool is_mac() { return ImGui::GetIO().ConfigMacOSXBehaviors; }

// "Cmd+S" on macOS, "Ctrl+S" elsewhere. `alt` adds Opt/Alt, `shift` adds Shift.
std::string chord(const char* key, bool shift = false, bool alt = false) {
    std::string s = is_mac() ? "Cmd+" : "Ctrl+";
    if (alt) s += is_mac() ? "Opt+" : "Alt+";
    if (shift) s += "Shift+";
    return s + key;
}

ImU32 with_alpha(ImU32 c, float a) {
    const auto alpha = static_cast<ImU32>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

void spinner_at(ImDrawList* dl, ImVec2 center, float radius, ImU32 color) {
    const float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(center, radius, t, t + 4.4f, 20);
    dl->PathStroke(color, std::max(1.5f, radius * 0.3f));
}

// Unity-style toolbar button: icon + label, highlighted while `active`.
bool tool_button(const char* id, icons::Glyph glyph, ImU32 icon_color, const char* label, bool enabled,
                 bool active, const std::string& tooltip) {
    const float h = ImGui::GetFrameHeight();
    const float icon = std::floor(ImGui::GetFontSize() * 0.9f);
    const float pad = std::floor(ImGui::GetStyle().FramePadding.x * 1.5f);
    const float w = pad + icon + 6.0f + ImGui::CalcTextSize(label).x + pad;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::color(Color::Selection));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color(Color::Selection));
    }
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(id, ImVec2(w, h));
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.0f : ImGui::GetStyle().DisabledAlpha;
    icons::draw(dl, ImVec2(p.x + pad, std::floor(p.y + (h - icon) * 0.5f)), icon, glyph, with_alpha(icon_color, alpha));
    dl->AddText(ImVec2(p.x + pad + icon + 6.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f),
                with_alpha(theme::color(Color::Text), alpha), label);
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip.c_str());
    return clicked;
}

float tool_button_width(const char* label) {
    const float icon = std::floor(ImGui::GetFontSize() * 0.9f);
    const float pad = std::floor(ImGui::GetStyle().FramePadding.x * 1.5f);
    return pad + icon + 6.0f + ImGui::CalcTextSize(label).x + pad;
}

// Number of code points in the first `bytes` bytes of a UTF-8 string.
int utf8_columns(const std::string& s, int bytes) {
    int n = 0;
    for (int i = 0; i < bytes && i < static_cast<int>(s.size()); ++i)
        if ((static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) != 0x80) ++n;
    return n;
}

std::string first_line(const std::string& s) {
    const size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

fs::path expand_user(const std::string& s) {
    if (s.size() >= 1 && s[0] == '~' && (s.size() == 1 || s[1] == '/' || s[1] == '\\')) {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home) home = std::getenv("USERPROFILE");
#endif
        if (home) return fs::path(home) / s.substr(s.size() > 1 ? 2 : 1);
    }
    return fs::path(s);
}

void center_next_window() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

} // namespace

// ---- Lifetime -------------------------------------------------------------------

EditorApp::EditorApp() {
    panels_.push_back(std::make_unique<HierarchyPanel>());
    panels_.push_back(std::make_unique<SettingsPanel>());
    panels_.push_back(std::make_unique<CodePanel>());
    panels_.back()->visible = false;
    panels_.push_back(std::make_unique<InspectorPanel>());
    panels_.push_back(std::make_unique<ProjectPanel>());
    panels_.push_back(std::make_unique<ConsolePanel>());
}

EditorApp::~EditorApp() {
    panels_.clear();      // panels may reference the context while destroyed
    ctx_.runner.cancel();
}

Panel* EditorApp::find_panel(std::string_view title) {
    for (auto& p : panels_)
        if (title == p->title()) return p.get();
    return nullptr;
}

bool EditorApp::any_modified() const { return ctx_.docs.any_modified(); }

bool EditorApp::open_project(const fs::path& location, const fs::path& engine_root, std::string* error) {
    std::string err;
    std::unique_ptr<Project> project = Project::open(location, engine_root, ctx_.docs, ctx_.log, &err);
    if (!project) {
        if (err.empty()) err = "unknown error";
        ctx_.log.error("Cannot open project " + location.string() + ": " + err);
        if (error) *error = err;
        return false;
    }
    ctx_.selection.clear();
    ctx_.project = std::move(project);
    ctx_.engine_root = engine_root;
    // Close the previous project's files (the new main.cpp stays open).
    Document* main_doc = ctx_.project->main_document();
    std::vector<Document*> stale;
    for (const auto& doc : ctx_.docs.documents())
        if (doc.get() != main_doc) stale.push_back(doc.get());
    for (Document* doc : stale) ctx_.docs.close(doc);
    if (main_doc) ctx_.docs.set_active(main_doc);
    ctx_.selection = ctx_.project->root().id;  // show the OS itself in the Inspector (Project::open logs "Opened ...")
    return true;
}

void EditorApp::request_quit(bool force) {
    if (force || !any_modified())
        quit_confirmed_ = true;
    else
        open_quit_prompt_ = true;
}

std::string EditorApp::window_title() const {
    std::string title = "OSEF Editor";
    if (ctx_.project) title += " \xE2\x80\x94 " + ctx_.project->name() + " \xE2\x80\x94 " + ctx_.project->main_file().string();
    if (any_modified()) title += " \xE2\x97\x8F";
    return title;
}

// ---- Settings (panel visibility in imgui.ini) ------------------------------------

void EditorApp::install_settings_handler() {
    ImGuiSettingsHandler handler;
    handler.TypeName = kSettingsType;
    handler.TypeHash = ImHashStr(kSettingsType);
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* h, const char*) -> void* { return h->UserData; };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        auto* app = static_cast<EditorApp*>(entry);
        const char* eq = std::strchr(line, '=');
        if (!eq) return;
        const std::string_view key(line, static_cast<size_t>(eq - line));
        const std::string_view value(eq + 1);
        if (key == "Experience") {
            app->ctx_.beginner_mode = value != "advanced";
            return;
        }
        if (key == "Theme") {
            if (value == "light") theme::set_palette(theme::Palette::Light);
            else if (value == "high_contrast") theme::set_palette(theme::Palette::HighContrast);
            else theme::set_palette(theme::Palette::Dark);
            return;
        }
        if (Panel* p = app->find_panel(key))
            p->visible = std::atoi(eq + 1) != 0;
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
        auto* app = static_cast<EditorApp*>(h->UserData);
        out->appendf("[%s][Visibility]\n", h->TypeName);
        out->appendf("Experience=%s\n", app->ctx_.beginner_mode ? "beginner" : "advanced");
        const char* palette = "dark";
        if (theme::palette() == theme::Palette::Light) palette = "light";
        else if (theme::palette() == theme::Palette::HighContrast) palette = "high_contrast";
        out->appendf("Theme=%s\n", palette);
        for (const auto& p : app->panels()) out->appendf("%s=%d\n", p->title(), p->visible ? 1 : 0);
        out->append("\n");
    };
    ImGui::AddSettingsHandler(&handler);
}

// ---- Commands ----------------------------------------------------------------------

void EditorApp::save_active() {
    Document* doc = ctx_.docs.active();
    if (!doc) return;
    if (doc->read_only()) {
        ctx_.log.warn(doc->title() + " is read-only");
        return;
    }
    std::string error;
    if (doc->save(&error))
        ctx_.log.info("Saved " + doc->path().string());
    else
        ctx_.log.error("Cannot save " + doc->path().string() + ": " + error);
}

void EditorApp::save_all() {
    int count = 0;
    for (const auto& doc : ctx_.docs.documents())
        if (doc->modified()) ++count;
    if (count == 0) return;
    std::string error;
    if (ctx_.docs.save_all(&error))
        ctx_.log.info("Saved " + std::to_string(count) + (count == 1 ? " file" : " files"));
    else
        ctx_.log.error("Save All failed: " + error);
}

void EditorApp::start_target(const std::string& id) {
    if (!ctx_.project) return;
    for (const BuildTarget& t : ctx_.project->build_targets()) {
        if (t.id == id) {
            ctx_.run_target(t);
            return;
        }
    }
    ctx_.log.warn("No build target '" + id + "'");
}

void EditorApp::edit_command(int command) {
    Document* doc = ctx_.docs.active();
    if (!doc) return;
    TextBuffer& b = doc->buffer();
    const bool writable = !doc->read_only();
    switch (command) {
    case kUndo:
        if (writable) b.undo();
        break;
    case kRedo:
        if (writable) b.redo();
        break;
    case kCut:
        if (writable && b.has_selection()) {
            ImGui::SetClipboardText(b.selected_text().c_str());
            b.backspace();
        }
        break;
    case kCopy:
        if (b.has_selection()) ImGui::SetClipboardText(b.selected_text().c_str());
        break;
    case kPaste:
        if (writable) {
            const char* text = ImGui::GetClipboardText();
            if (text && *text) {
                b.break_undo_group();
                b.insert(text);
                b.break_undo_group();
            }
        }
        break;
    case kSelectAll: b.select_all(); break;
    case kFind:
        // The find bar belongs to the code panel: focus it, then forward the
        // same Cmd/Ctrl+F chord the user would press (see frame()).
        find_inject_ = 3;
        break;
    default: break;
    }
    ctx_.request_focus_code = true;
}

void EditorApp::handle_shortcuts() {
    // Modal dialogs own the keyboard.
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        ImGui::GetTopMostPopupModal() != nullptr)
        return;
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) save_active();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S, global)) save_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) open_project_dialog_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) open_new_os_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, global)) request_quit();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) start_target("run");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global)) start_target("iso");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) start_target("qemu");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Period, global)) ctx_.runner.cancel();
}

// ---- Frame -------------------------------------------------------------------------

void EditorApp::frame() {
    if (ctx_.project) ctx_.project->sync_from_code();
    ctx_.runner.poll();

    handle_shortcuts();
    draw_menu_bar();
    draw_toolbar();
    draw_status_bar();
    draw_dockspace();
    draw_panels();
    draw_dialogs();

    // Edit > Find: wait a frame for the code panel to take focus, then press
    // and release Cmd/Ctrl+F (macOS swaps Cmd/Ctrl inside ImGui, hence Super).
    if (find_inject_ > 0) {
        ImGuiIO& io = ImGui::GetIO();
        const ImGuiKey mod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        if (find_inject_ == 2) {
            io.AddKeyEvent(mod, true);
            io.AddKeyEvent(ImGuiKey_F, true);
        } else if (find_inject_ == 1) {
            io.AddKeyEvent(ImGuiKey_F, false);
            io.AddKeyEvent(mod, false);
        }
        --find_inject_;
    }
}

void EditorApp::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    Document* doc = ctx_.docs.active();
    const bool running = ctx_.runner.running();

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New OS\xE2\x80\xA6", chord("N").c_str())) open_new_os_ = true;
        if (ImGui::MenuItem("Open Project\xE2\x80\xA6", chord("O").c_str())) open_project_dialog_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Save", chord("S").c_str(), false, doc && !doc->read_only())) save_active();
        if (ImGui::MenuItem("Save All", chord("S", false, true).c_str(), false, any_modified())) save_all();
        if (ImGui::MenuItem("Revert", nullptr, false, doc && doc->modified())) open_revert_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", chord("Q").c_str())) request_quit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const bool writable = doc && !doc->read_only();
        const bool sel = doc && doc->buffer().has_selection();
        if (ImGui::MenuItem("Undo", chord("Z").c_str(), false, writable && doc->buffer().can_undo()))
            edit_command(kUndo);
        if (ImGui::MenuItem("Redo", is_mac() ? chord("Z", true).c_str() : chord("Y").c_str(), false,
                            writable && doc->buffer().can_redo()))
            edit_command(kRedo);
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", chord("X").c_str(), false, writable && sel)) edit_command(kCut);
        if (ImGui::MenuItem("Copy", chord("C").c_str(), false, sel)) edit_command(kCopy);
        if (ImGui::MenuItem("Paste", chord("V").c_str(), false, writable)) edit_command(kPaste);
        if (ImGui::MenuItem("Select All", chord("A").c_str(), false, doc != nullptr)) edit_command(kSelectAll);
        ImGui::Separator();
        if (ImGui::MenuItem("Find\xE2\x80\xA6", chord("F").c_str(), false, doc != nullptr)) edit_command(kFind);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Component")) {
        Project* project = ctx_.project.get();
        const bool has_decl = project && project->hardware().found;
        if (ImGui::MenuItem("Add Hardware Declaration", nullptr, false, project && !has_decl)) {
            project->add_hardware_declaration();
            ctx_.open_file(project->main_file(), project->hardware().line);
        }
        if (project && has_decl && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("main.cpp already declares its hardware (line %d)", project->hardware().line);
        Component* c = ctx_.selected();
        const bool planned = c && c->status == ComponentStatus::Planned;
        if (ImGui::MenuItem("Create Driver Stub\xE2\x80\xA6", nullptr, false, project && planned)) {
            std::string error;
            const fs::path stub = project->create_driver_stub(*c, &error);
            if (stub.empty()) {
                ctx_.log.error("Cannot create a driver stub for " + c->name + ": " + error);
            } else {
                ctx_.log.info("Created " + stub.string());
                ctx_.open_file(stub);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Writes drivers/<id>.cpp for a Planned component as a starting point");
        ImGui::Separator();
        const std::string toggle_label =
            c && c->toggleable ? (c->enabled ? "Disable " : "Enable ") + c->name : std::string("Enable / Disable");
        if (ImGui::MenuItem(toggle_label.c_str(), nullptr, false, project && c && c->toggleable)) {
            c->enabled = !c->enabled;
            project->apply_enabled(*c);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Build")) {
        if (ctx_.project) {
            for (const BuildTarget& t : ctx_.project->build_targets()) {
                std::string sc;
                if (t.id == "run") sc = chord("P");
                if (t.id == "iso") sc = chord("B");
                if (t.id == "qemu") sc = chord("P", true);
                if (ImGui::MenuItem(t.label.c_str(), sc.empty() ? nullptr : sc.c_str(), false, !running))
                    ctx_.run_target(t);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", (t.tooltip.empty() ? t.command : t.tooltip + "\n" + t.command).c_str());
            }
        } else {
            ImGui::MenuItem("(no project open)", nullptr, false, false);
        }
        ImGui::Separator();
        const std::string cancel = running ? "Cancel " + ctx_.runner.current_label() : std::string("Cancel");
        if (ImGui::MenuItem(cancel.c_str(), chord(".").c_str(), false, running)) ctx_.runner.cancel();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        for (auto& p : panels_)
            if (ImGui::MenuItem(p->title(), nullptr, &p->visible)) ImGui::MarkIniSettingsDirty();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            for (auto& p : panels_) p->visible = true;
            layout_reset_pending_ = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Appearance")) {
        if (ImGui::BeginMenu("Experience")) {
            if (ImGui::MenuItem("Beginner guidance", nullptr, ctx_.beginner_mode)) {
                ctx_.beginner_mode = true;
                if (Panel* code = find_panel("Code")) code->visible = false;
                ImGui::MarkIniSettingsDirty();
            }
            if (ImGui::MenuItem("Advanced", nullptr, !ctx_.beginner_mode)) {
                ctx_.beginner_mode = false;
                if (Panel* code = find_panel("Code")) code->visible = true;
                ImGui::MarkIniSettingsDirty();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Color theme")) {
            for (const theme::Palette palette : {theme::Palette::Dark, theme::Palette::Light,
                                                 theme::Palette::HighContrast}) {
                if (ImGui::MenuItem(theme::palette_name(palette), nullptr, theme::palette() == palette)) {
                    theme::set_palette(palette);
                    ImGui::MarkIniSettingsDirty();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About OSEF\xE2\x80\xA6")) open_about_ = true;
        if (ImGui::MenuItem("Keyboard Shortcuts\xE2\x80\xA6")) open_shortcuts_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorApp::draw_toolbar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float frame_h = ImGui::GetFrameHeight();
    const float height = std::floor(frame_h + 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color(Color::TitleBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    const bool open = ImGui::BeginViewportSideBar("##OSEFToolbar", vp, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 1.0f), ImVec2(wp.x + ws.x, wp.y + ws.y - 1.0f),
                    theme::color(Color::Separator));
        const float y = std::floor((height - frame_h) * 0.5f);

        // Center: Run / Build ISO / QEMU (+ spinner and Stop while a job runs).
        Project* project = ctx_.project.get();
        const std::vector<BuildTarget> targets = project ? project->build_targets() : std::vector<BuildTarget>{};
        const bool running = ctx_.runner.running();
        const std::string running_label = ctx_.runner.current_label();
        struct Btn {
            const char* target;
            const char* id;
            const char* label;
            icons::Glyph glyph;
            ImU32 color;
        };
        const Btn buttons[] = {
            {"run", "##tb_run", "Run", icons::Glyph::Play, theme::color(Color::Implemented)},
            {"iso", "##tb_iso", "Build ISO", icons::Glyph::Build, theme::color(Color::Text)},
            {"qemu", "##tb_qemu", "QEMU", icons::Glyph::Play, IM_COL32(0x7F, 0xA8, 0xE8, 0xFF)},
        };
        const float gap = 4.0f;
        float group_w = 0.0f;
        for (const Btn& b : buttons) group_w += tool_button_width(b.label) + gap;
        group_w -= gap;
        float x = std::floor((ws.x - group_w) * 0.5f);
        for (const Btn& b : buttons) {
            const BuildTarget* t = nullptr;
            for (const BuildTarget& bt : targets)
                if (bt.id == b.target) t = &bt;
            const bool active = running && t && t->label == running_label;
            std::string tip = t ? t->label + "\n" + t->command : std::string("Not available");
            if (t && std::strcmp(b.target, "run") == 0) tip += "\n" + chord("P");
            if (t && std::strcmp(b.target, "iso") == 0) tip += "\n" + chord("B");
            if (t && std::strcmp(b.target, "qemu") == 0) tip += "\n" + chord("P", true);
            ImGui::SetCursorPos(ImVec2(x, y));
            if (tool_button(b.id, b.glyph, b.color, b.label, t && !running, active, tip) && t) ctx_.run_target(*t);
            x += tool_button_width(b.label) + gap;
        }
        if (running) {
            const float r = std::floor(frame_h * 0.32f);
            spinner_at(dl, ImVec2(wp.x + x + 6.0f + r, wp.y + y + frame_h * 0.5f), r, theme::color(Color::Accent));
            ImGui::SetCursorPos(ImVec2(x + 12.0f + r * 2.0f, y));
            if (tool_button("##tb_stop", icons::Glyph::Stop, theme::color(Color::Error), "Stop", true, false,
                            "Cancel " + running_label + " (" + chord(".") + ")"))
                ctx_.runner.cancel();
        }

        // Right: the declared machine, or a red warning when there is none.
        if (project) {
            const HardwareParse& hw = project->hardware();
            std::string badge;
            ImU32 text_color = theme::color(Color::TextDim);
            ImU32 bg = theme::color(Color::InputBg);
            if (hw.found) {
                badge = (hw.decl.arch.empty() ? "?" : hw.decl.arch) + " \xC2\xB7 " +
                        (hw.decl.firmware.empty() ? "?" : hw.decl.firmware);
            } else {
                badge = "No hardware declared";
                text_color = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
                bg = theme::color(Color::Error);
            }
            const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
            const float icon = std::floor(ImGui::GetFontSize() * 0.9f);
            const bool problems = hw.found && !hw.problems.empty();
            const float bw = ts.x + 16.0f + (problems ? icon + 6.0f : 0.0f);
            const float bh = frame_h;
            const float bx = ws.x - bw - 10.0f;
            if (bx > x + 20.0f) {
                ImGui::SetCursorPos(ImVec2(bx, y));
                ImGui::InvisibleButton("##hw_badge", ImVec2(bw, bh));
                const ImVec2 p = ImGui::GetItemRectMin();
                dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), bg, 3.0f);
                if (hw.found) dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), theme::color(Color::Border), 3.0f);
                float tx = p.x + 8.0f;
                if (problems) {
                    icons::draw(dl, ImVec2(tx, std::floor(p.y + (bh - icon) * 0.5f)), icon, icons::Glyph::Warning,
                                theme::color(Color::Warning));
                    tx += icon + 6.0f;
                }
                dl->AddText(ImVec2(tx, p.y + (bh - ts.y) * 0.5f), text_color, badge.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                    ImGui::BeginTooltip();
                    if (hw.found) {
                        ImGui::Text("OSEF_HARDWARE in %s, line %d", project->main_file().filename().string().c_str(),
                                    hw.line);
                        ImGui::Text("Memory %u MiB", hw.decl.memory_mb);
                        for (const std::string& problem : hw.problems)
                            ImGui::TextColored(theme::color_vec(Color::Warning), "%s", problem.c_str());
                        ImGui::TextDisabled("Click to show the declaration");
                    } else {
                        ImGui::TextUnformatted("main.cpp has no OSEF_HARDWARE(...) declaration,");
                        ImGui::TextUnformatted("so the OS will not link. Click to add one.");
                    }
                    ImGui::EndTooltip();
                }
                if (ImGui::IsItemClicked()) {
                    if (hw.found) {
                        ctx_.open_file(project->main_file(), hw.line);
                    } else {
                        ImGui::OpenPopup("##add_hw");
                    }
                }
                if (ImGui::BeginPopup("##add_hw")) {
                    if (ImGui::MenuItem("Add Hardware Declaration")) {
                        project->add_hardware_declaration();
                        ctx_.open_file(project->main_file(), project->hardware().line);
                    }
                    ImGui::EndPopup();
                }
            }
        }
    }
    ImGui::End();
}

void EditorApp::draw_status_bar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = std::floor(ImGui::GetFrameHeight() + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color(Color::TitleBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    const bool open = ImGui::BeginViewportSideBar("##OSEFStatusBar", vp, ImGuiDir_Down, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddLine(wp, ImVec2(wp.x + ws.x, wp.y), theme::color(Color::Separator));
        const float font = ImGui::GetFontSize();
        const float text_y = wp.y + std::floor((ws.y - font) * 0.5f);
        const float icon = std::floor(font * 0.9f);

        // Right: active document position (+ unsaved mark).
        float right = wp.x + ws.x - 10.0f;
        if (Document* doc = ctx_.docs.active()) {
            const TextBuffer& b = doc->buffer();
            const TextPos cur = b.cursor();
            char pos[64];
            std::snprintf(pos, sizeof(pos), "Ln %d, Col %d", cur.line + 1, utf8_columns(b.line(cur.line), cur.col) + 1);
            const std::string text = doc->title() + "  \xE2\x80\x94  " + pos + (doc->modified() ? "  \xE2\x97\x8F" : "");
            const float w = ImGui::CalcTextSize(text.c_str()).x;
            right -= w;
            dl->AddText(ImVec2(right, text_y), theme::color(Color::TextDim), text.c_str());
            right -= 24.0f;
        }

        // Middle-right: the running job.
        if (ctx_.runner.running()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  %.1f s", ctx_.runner.elapsed_seconds());
            const std::string text = ctx_.runner.current_label() + buf;
            const float w = ImGui::CalcTextSize(text.c_str()).x;
            right -= w;
            dl->AddText(ImVec2(right, text_y), theme::color(Color::Text), text.c_str());
            const float r = std::floor(font * 0.32f);
            spinner_at(dl, ImVec2(right - r - 6.0f, text_y + font * 0.5f), r, theme::color(Color::Accent));
            right -= r * 2.0f + 30.0f;
        }

        // Left: last log line, clickable to show the Console.
        const LogLine last = ctx_.log.last();
        float x = wp.x + 8.0f;
        const float left_w = std::max(0.0f, right - x);
        ImGui::SetCursorScreenPos(ImVec2(x, wp.y + 1.0f));
        ImGui::InvisibleButton("##status_log", ImVec2(std::max(1.0f, left_w), ws.y - 2.0f));
        if (ImGui::IsItemClicked()) ctx_.request_focus_console = true;
        dl->PushClipRect(ImVec2(x, wp.y), ImVec2(x + left_w, wp.y + ws.y), true);
        if (last.serial == 0) {
            dl->AddText(ImVec2(x, text_y), theme::color(Color::TextDim), "Ready");
        } else {
            icons::Glyph glyph = icons::Glyph::Info;
            ImU32 color = theme::color(Color::Info);
            if (last.level == LogLevel::Warning) {
                glyph = icons::Glyph::Warning;
                color = theme::color(Color::Warning);
            } else if (last.level == LogLevel::Error) {
                glyph = icons::Glyph::Error;
                color = theme::color(Color::Error);
            } else if (last.level == LogLevel::Build) {
                glyph = icons::Glyph::Build;
                color = theme::color(Color::TextDim);
            }
            icons::draw(dl, ImVec2(x, std::floor(wp.y + (ws.y - icon) * 0.5f)), icon, glyph, color);
            const std::string text = first_line(last.text);
            dl->AddText(ImVec2(x + icon + 6.0f, text_y), theme::color(Color::Text), text.c_str());
        }
        dl->PopClipRect();
        if (last.serial != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Click to show the Console");
    }
    ImGui::End();
}

void EditorApp::draw_dockspace() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID id = ImHashStr(kDockspaceName);
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(id);
    const bool fresh = node == nullptr || (!layout_built_ && node->IsLeafNode() && node->Windows.Size == 0);
    if (layout_reset_pending_ || fresh) {
        build_default_layout(id, vp->WorkSize.x, vp->WorkSize.y);
        layout_reset_pending_ = false;
    }
    layout_built_ = true;
    // The 2 px gaps between docked panels show the host's dark background.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color(Color::Separator));
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_None);
    ImGui::PopStyleColor();
}

void EditorApp::build_default_layout(unsigned int dockspace_id, float width, float height) {
    // Inspector: full height on the right (24%). Below Hierarchy + Code: a
    // bottom strip (26%) with Project + Console tabs. Hierarchy: left 19%.
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(width, height));
    ImGuiID right = 0, rest = 0, bottom = 0, top = 0, left = 0, center = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.24f, &right, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.26f, &bottom, &top);
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.19f / 0.76f, &left, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("OS Settings", center);
    ImGui::DockBuilderDockWindow("Code", center);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Project", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderFinish(dockspace_id);
    focus_console_tab_frames_ = 2;
}

void EditorApp::draw_panels() {
    // Focus requests from panels/context (consumed once per frame).
    const bool focus_console = ctx_.request_focus_console;
    const bool focus_code = ctx_.request_focus_code;
    const bool focus_inspector = ctx_.request_focus_inspector;
    ctx_.request_focus_console = ctx_.request_focus_code = ctx_.request_focus_inspector = false;

    for (auto& panel : panels_) {
        const std::string_view title = panel->title();
        bool focus = (focus_console && title == "Console") || (focus_code && title == "Code") ||
                     (focus_inspector && title == "Inspector");
        if (focus) panel->visible = true;
        if (!panel->visible) continue;
        if (focus) ImGui::SetNextWindowFocus();

        if (panel->zero_padding()) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool was_visible = panel->visible;
        const bool open = ImGui::Begin(panel->title(), &panel->visible, panel->window_flags());
        if (panel->zero_padding()) ImGui::PopStyleVar();
        if (was_visible != panel->visible) ImGui::MarkIniSettingsDirty();
        if (open) {
            // A panel that throws must not leave ImGui's stacks unbalanced.
            ImGuiErrorRecoveryState state;
            ImGui::ErrorRecoveryStoreState(&state);
            try {
                panel->draw(ctx_);
            } catch (const std::exception& e) {
                ImGuiIO& io = ImGui::GetIO();
                const bool asserts = io.ConfigErrorRecoveryEnableAssert;
                io.ConfigErrorRecoveryEnableAssert = false;
                ImGui::ErrorRecoveryTryToRecoverState(&state);
                io.ConfigErrorRecoveryEnableAssert = asserts;
                const std::string message = std::string(panel->title()) + " panel error: " + e.what();
                if (message != last_panel_error_) ctx_.log.error(message);
                last_panel_error_ = message;
            }
        }
        ImGui::End();
    }

    // After (re)building the layout, bring the Console tab to the front of
    // the bottom strip, then give keyboard focus back to the code editor.
    if (focus_console_tab_frames_ > 0) {
        if (--focus_console_tab_frames_ == 0) {
            ImGui::SetWindowFocus("Console");
            ImGui::SetWindowFocus(ctx_.beginner_mode ? "OS Settings" : "Code");
        }
    }
}

// ---- Dialogs -----------------------------------------------------------------------

// File > New OS: a one-page wizard for people who have never written an OS.
void EditorApp::draw_new_os_dialog() {
    static const unsigned kMemory[] = {64, 128, 256, 512};
    if (open_new_os_) {
        open_new_os_ = false;
        new_os_error_.clear();
        if (!new_os_folder_edited_) new_os_folder_ = default_project_folder(new_os_name_).string();
        ImGui::OpenPopup("Create a New OS");
    }
    center_next_window();
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Create a New OS", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    const float label_w = 170.0f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1.0f);
    };
    ImGui::TextWrapped("Scaffolds a new OSEF project: main.cpp with an OSEF_HARDWARE declaration, plus "
                       "osef-settings.ini and the generated osef_generated.cpp. Everything is editable later.");
    ImGui::Spacing();

    label("1. OS name");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##new_name", &new_os_name_) && !new_os_folder_edited_)
        new_os_folder_ = default_project_folder(new_os_name_).string();
    label("   Project directory");
    if (ImGui::InputText("##new_folder", &new_os_folder_)) new_os_folder_edited_ = true;

    label("2. Boot firmware");
    if (ImGui::RadioButton("UEFI (x86_64)", new_os_uefi_)) new_os_uefi_ = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Legacy BIOS", !new_os_uefi_)) new_os_uefi_ = false;
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("UEFI is the default for modern hardware and QEMU+OVMF; BIOS for legacy/CSM machines.");

    label("3. Devices");
    ImGui::Checkbox("Framebuffer", &new_os_screen_);
    ImGui::SameLine();
    ImGui::Checkbox("PS/2 kbd", &new_os_keyboard_);
    ImGui::SameLine();
    ImGui::Checkbox("COM1 UART", &new_os_serial_);
    ImGui::SameLine();
    ImGui::Checkbox("CMOS RTC", &new_os_clock_);
    ImGui::SameLine();
    ImGui::Checkbox("PCI", &new_os_pci_);
    label("   Minimum RAM");
    static const char* const memory_labels[] = {"64 MiB", "128 MiB (default)", "256 MiB", "512 MiB"};
    ImGui::Combo("##new_mem", &new_os_memory_, memory_labels, 4);

    label("4. Console theme");
    const auto& looks = look_presets();
    new_os_look_ = std::clamp(new_os_look_, 0, static_cast<int>(looks.size()) - 1);
    if (ImGui::BeginCombo("##new_look", looks[static_cast<size_t>(new_os_look_)].name.c_str())) {
        for (size_t i = 0; i < looks.size(); ++i) {
            if (ImGui::Selectable(looks[i].name.c_str(), static_cast<int>(i) == new_os_look_))
                new_os_look_ = static_cast<int>(i);
            ImGui::SetItemTooltip("%s", looks[i].description.c_str());
        }
        ImGui::EndCombo();
    }
    label("5. Templates");
    ImGui::Checkbox("Generate sample programs (hello, guess, countdown, clock)", &new_os_samples_);

    NewProjectOptions o;
    o.name = new_os_name_;
    o.folder = expand_user(new_os_folder_);
    o.firmware = new_os_uefi_ ? "UEFI" : "BIOS";
    o.screen = new_os_screen_;
    o.keyboard = new_os_keyboard_;
    o.serial = new_os_serial_;
    o.clock = new_os_clock_;
    o.pci = new_os_pci_;
    o.memory_mb = kMemory[std::clamp(new_os_memory_, 0, 3)];
    o.look = looks[static_cast<size_t>(new_os_look_)].name;
    o.sample_programs = new_os_samples_;

    const std::vector<std::string> problems = validate_new_project(o);
    ImGui::Spacing();
    for (const std::string& p : problems) ImGui::TextColored(theme::color_vec(Color::Warning), "%s", p.c_str());
    if (!new_os_error_.empty()) ImGui::TextColored(theme::color_vec(Color::Error), "%s", new_os_error_.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(!problems.empty());
    if (ImGui::Button("Create project", ImVec2(160, 0))) {
        std::string error;
        const fs::path main = create_new_project(o, &error);
        if (main.empty()) {
            new_os_error_ = error;
        } else if (!ctx_.docs.save_all(&error)) {
            new_os_error_ = "Could not save open files first: " + error;
        } else if (!open_project(o.folder, ctx_.engine_root, &error)) {
            new_os_error_ = error;
        } else {
            ctx_.log.info("Created " + o.name + " in " + o.folder.string());
            ctx_.beginner_mode = true;
            if (Panel* settings = find_panel("OS Settings")) settings->visible = true;
            ImGui::SetWindowFocus("OS Settings");
            new_os_folder_edited_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}


void EditorApp::draw_dialogs() {
    const ImGuiWindowFlags modal = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    // Quit with unsaved changes.
    if (open_quit_prompt_) {
        ImGui::OpenPopup("Unsaved Changes##quit");
        open_quit_prompt_ = false;
    }
    center_next_window();
    if (ImGui::BeginPopupModal("Unsaved Changes##quit", nullptr, modal)) {
        ImGui::PushFont(ctx_.fonts.ui_bold, 0.0f);
        ImGui::TextUnformatted("Save changes before quitting?");
        ImGui::PopFont();
        ImGui::Spacing();
        for (const auto& doc : ctx_.docs.documents())
            if (doc->modified()) ImGui::BulletText("%s", doc->path().string().c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Your changes will be lost if you don't save them.");
        ImGui::Separator();
        if (ImGui::Button("Save All", ImVec2(110, 0))) {
            std::string error;
            if (ctx_.docs.save_all(&error)) {
                quit_confirmed_ = true;
                ImGui::CloseCurrentPopup();
            } else {
                ctx_.log.error("Save All failed: " + error);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            quit_confirmed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    draw_new_os_dialog();

    // File > Open Project.
    if (open_project_dialog_) {
        const std::string current = ctx_.project ? ctx_.project->directory().string() : std::string();
        std::snprintf(project_path_, sizeof(project_path_), "%s", current.c_str());
        project_dialog_error_.clear();
        ImGui::OpenPopup("Open Project");
        open_project_dialog_ = false;
    }
    center_next_window();
    if (ImGui::BeginPopupModal("Open Project", nullptr, modal)) {
        ImGui::TextUnformatted("OS project directory (containing main.cpp), or a .cpp file:");
        ImGui::SetNextItemWidth(560.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##project_path", project_path_, sizeof(project_path_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (!ctx_.engine_root.empty())
            ImGui::TextDisabled("Engine: %s", ctx_.engine_root.string().c_str());
        if (!project_dialog_error_.empty())
            ImGui::TextColored(theme::color_vec(Color::Error), "%s", project_dialog_error_.c_str());
        const bool dirty = any_modified();
        if (dirty) ImGui::TextColored(theme::color_vec(Color::Warning), "Unsaved changes will be saved first.");
        ImGui::Separator();
        if (ImGui::Button(dirty ? "Save All & Open" : "Open", ImVec2(130, 0)) || enter) {
            std::string error;
            if (dirty && !ctx_.docs.save_all(&error)) {
                project_dialog_error_ = "Save failed: " + error;
            } else if (open_project(expand_user(project_path_), ctx_.engine_root, &error)) {
                ImGui::CloseCurrentPopup();
            } else {
                project_dialog_error_ = error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // File > Revert.
    if (open_revert_) {
        ImGui::OpenPopup("Revert File");
        open_revert_ = false;
    }
    center_next_window();
    if (ImGui::BeginPopupModal("Revert File", nullptr, modal)) {
        Document* doc = ctx_.docs.active();
        if (!doc) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("Discard all unsaved changes to %s?", doc->title().c_str());
            ImGui::Separator();
            if (ImGui::Button("Revert", ImVec2(110, 0))) {
                std::string error;
                if (doc->reload(&error))
                    ctx_.log.info("Reverted " + doc->path().string());
                else
                    ctx_.log.error("Cannot revert " + doc->path().string() + ": " + error);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Help > About.
    if (open_about_) {
        ImGui::OpenPopup("About OSEF");
        open_about_ = false;
    }
    center_next_window();
    if (ImGui::BeginPopupModal("About OSEF", nullptr, modal)) {
        ImGui::PushFont(ctx_.fonts.ui_bold, ImGui::GetStyle().FontSizeBase * 1.4f);
        ImGui::TextUnformatted("OSEF Editor");
        ImGui::PopFont();
        ImGui::TextUnformatted("OSEF is an operating system engine: like a game engine, but you build");
        ImGui::TextUnformatted("operating systems. The editor shows your OS as components, keeps the");
        ImGui::TextUnformatted("OSEF_HARDWARE declaration in sync with the Inspector, and runs builds.");
        ImGui::Spacing();
        if (ctx_.project) {
            ImGui::TextDisabled("Project");
            ImGui::SameLine(90);
            ImGui::TextUnformatted(ctx_.project->directory().string().c_str());
        }
        ImGui::TextDisabled("Engine");
        ImGui::SameLine(90);
        ImGui::TextUnformatted(ctx_.engine_root.empty() ? "(not found)" : ctx_.engine_root.string().c_str());
        ImGui::TextDisabled("Built with");
        ImGui::SameLine(90);
        ImGui::Text("Dear ImGui %s, GLFW, OpenGL 3.2", IMGUI_VERSION);
        ImGui::TextDisabled("Fonts");
        ImGui::SameLine(90);
        ImGui::TextUnformatted("Inter and JetBrains Mono (SIL Open Font License 1.1)");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Help > Keyboard Shortcuts.
    if (open_shortcuts_) {
        ImGui::OpenPopup("Keyboard Shortcuts");
        open_shortcuts_ = false;
    }
    center_next_window();
    if (ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, modal)) {
        struct Row {
            std::string keys;
            const char* what;
        };
        const Row rows[] = {
            {chord("S"), "Save the active file"},
            {chord("S", false, true), "Save all files"},
            {chord("O"), "Open a project"},
            {chord("P"), "Run (hosted)"},
            {chord("B"), "Build ISO"},
            {chord("P", true), "Run in QEMU"},
            {chord("."), "Cancel the running job"},
            {chord("Z"), "Undo (code editor)"},
            {is_mac() ? chord("Z", true) : chord("Y"), "Redo (code editor)"},
            {chord("X") + " / " + chord("C") + " / " + chord("V"), "Cut / Copy / Paste"},
            {chord("A"), "Select all"},
            {chord("F"), "Find in the active file"},
            {chord("C"), "Copy selected Console lines"},
            {chord("Q"), "Quit"},
        };
        if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 280.0f);
            for (const Row& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushFont(ctx_.fonts.mono, 0.0f);
                ImGui::TextUnformatted(r.keys.c_str());
                ImGui::PopFont();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.what);
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace osef::editor
