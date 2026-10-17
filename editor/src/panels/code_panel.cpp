// CodePanel: document tabs, a breadcrumb row with the read-only lock, an
// optional find bar and the CodeEditor for the active document.

#include "panels/code_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"
#include "text/cpp_highlighter.h"
#include "text/text_buffer.h"
#include "widgets/code_editor.h"

#include "imgui.h"
#include "imgui_internal.h"  // GetActiveID

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <cstdint>

namespace fs = std::filesystem;

namespace osef::editor {

namespace {

using theme::Color;

constexpr const char* kClosePopup = "Save Changes?##code_close";

// Per-document view state: the widget (scroll, drag, caret) and the highlighter cache.
struct View {
    CodeEditor editor;
    CppHighlighter hl;
};

fs::path canonical_or_self(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

// `path` relative to `base` if it lies inside it, else empty.
fs::path relative_inside(const fs::path& path, const fs::path& base) {
    if (base.empty()) return {};
    const fs::path rel = path.lexically_relative(base);
    if (rel.empty() || *rel.begin() == "..") return {};
    return rel;
}

bool is_cpp_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kExts[] = {".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp", ".s", ".m", ".mm"};
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

// A small flat icon button (Unity toolbar style). Returns true when clicked.
bool icon_button(const char* id, icons::Glyph glyph, float size, ImU32 color, const char* tooltip) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float pad = 3.0f;
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size + pad * 2.0f, size + pad * 2.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size + pad * 2.0f, pos.y + size + pad * 2.0f),
                          theme::color(ImGui::IsItemActive() ? Color::ButtonActive : Color::Hover), 3.0f);
        if (tooltip) ImGui::SetTooltip("%s", tooltip);
    }
    icons::draw(dl, ImVec2(pos.x + pad, pos.y + pad), size, glyph, color);
    return clicked;
}

// Find input callback: after Enter re-focuses the input, put the caret at the
// end instead of selecting everything, so typing continues the query.
int find_input_callback(ImGuiInputTextCallbackData* data) {
    bool* cursor_to_end = static_cast<bool*>(data->UserData);
    if (*cursor_to_end) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        *cursor_to_end = false;
    }
    return 0;
}

} // namespace

struct CodePanel::State {
    std::unordered_map<const Document*, std::unique_ptr<View>> views;
    const Document* last_active = nullptr;
    CodeEditorStyle style;

    // Find bar.
    bool find_open = false;
    bool find_focus = false;  // put keyboard focus into the find input on the next frame
    bool find_case = false;
    bool find_cursor_end = false;  // see find_input_callback
    char find_text[256] = {};
    uint64_t count_version = ~0ull;
    std::string count_needle;
    bool count_case = false;
    std::vector<TextPos> matches;

    // Close confirmation for a modified document.
    Document* pending_close = nullptr;
    bool open_close_popup = false;

    fs::path engine_dir;  // <engine_root>/engine, canonical
    fs::path engine_root;

    View& view_for(const Document* doc) {
        auto& v = views[doc];
        if (!v) {
            v = std::make_unique<View>();
            v->hl.set_plain_text(!is_cpp_file(doc->path()));
        }
        return *v;
    }
};

CodePanel::CodePanel() : state_(new State) {}
CodePanel::~CodePanel() { delete state_; }

namespace {

bool is_project_main(const EditorContext& ctx, const Document* doc) {
    return ctx.project && ctx.project->main_document() == doc;
}

// Saves `doc`, reporting to the console. Returns false on failure.
bool save_document(EditorContext& ctx, Document* doc) {
    std::string error;
    if (!doc->save(&error)) {
        ctx.log.error("Cannot save " + doc->path().string() + (error.empty() ? "" : ": " + error));
        return false;
    }
    ctx.log.info("Saved " + doc->path().string());
    return true;
}

} // namespace

void CodePanel::draw(EditorContext& ctx) {
    State& s = *state_;
    const auto& docs = ctx.docs.documents();

    if (s.engine_root != ctx.engine_root) {
        s.engine_root = ctx.engine_root;
        s.engine_dir = ctx.engine_root.empty() ? fs::path() : canonical_or_self(ctx.engine_root / "engine");
    }

    // Drop views of closed documents; engine sources open read-only.
    for (auto it = s.views.begin(); it != s.views.end();) {
        const bool open = std::any_of(docs.begin(), docs.end(), [&](const auto& d) { return d.get() == it->first; });
        it = open ? std::next(it) : s.views.erase(it);
    }
    for (const auto& d : docs) {
        if (s.views.count(d.get())) continue;
        if (!relative_inside(d->path(), s.engine_dir).empty()) d->set_read_only(true);
        s.view_for(d.get());
    }
    if (s.pending_close && std::none_of(docs.begin(), docs.end(), [&](const auto& d) { return d.get() == s.pending_close; }))
        s.pending_close = nullptr;

    // ---- empty state ----
    if (docs.empty()) {
        s.find_open = false;
        s.last_active = nullptr;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "Select a component in the Hierarchy to open its source";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const float icon = 36.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float cy = origin.y + std::max(0.0f, (avail.y - icon - ts.y - 12.0f) * 0.45f);
        icons::draw(ImGui::GetWindowDrawList(), ImVec2(origin.x + (avail.x - icon) * 0.5f, cy), icon,
                    icons::Glyph::FileCode, theme::color(Color::TextDisabled));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(0.0f, (avail.x - ts.x) * 0.5f), cy + icon + 12.0f));
        ImGui::TextColored(theme::color_vec(Color::TextDim), "%s", msg);
        return;
    }

    if (!ctx.docs.active()) ctx.docs.set_active(docs.front().get());
    const bool external_switch = ctx.docs.active() != s.last_active;

    // The Code window itself got focus (tab click, Window menu, open_file):
    // hand it to the editor unless something in the panel is being used.
    const bool window_focused = ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive();

    // ---- tabs ----
    std::vector<Document*> close_requests;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
    const ImVec2 bar_min = ImGui::GetCursorScreenPos();
    const float bar_h = ImGui::GetFrameHeight() + 4.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        bar_min, ImVec2(bar_min.x + ImGui::GetContentRegionAvail().x, bar_min.y + bar_h), theme::color(Color::TitleBg));
    ImGui::SetCursorScreenPos(ImVec2(bar_min.x + 4.0f, bar_min.y + 4.0f));
    const ImGuiTabBarFlags bar_flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
                                       ImGuiTabBarFlags_NoTooltip;
    if (ImGui::BeginTabBar("##documents", bar_flags)) {
        for (const auto& d : docs) {
            Document* doc = d.get();
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (doc->modified()) flags |= ImGuiTabItemFlags_UnsavedDocument;
            if (external_switch && doc == ctx.docs.active()) flags |= ImGuiTabItemFlags_SetSelected;
            const std::string label = doc->title() + "###" + doc->path().string();
            bool open = true;
            // The project's main.cpp stays open for the project's lifetime.
            const bool closable = !is_project_main(ctx, doc);
            const bool selected = ImGui::BeginTabItem(label.c_str(), closable ? &open : nullptr, flags);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s%s", doc->path().string().c_str(), doc->read_only() ? "  (read-only)" : "");
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Save", nullptr, false, doc->modified())) save_document(ctx, doc);
                if (ImGui::MenuItem("Close", nullptr, false, closable)) close_requests.push_back(doc);
                if (ImGui::MenuItem("Close Others")) {
                    for (const auto& other : docs)
                        if (other.get() != doc && !is_project_main(ctx, other.get())) close_requests.push_back(other.get());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(doc->path().string().c_str());
                ImGui::EndPopup();
            }
            if (selected) {
                if (!external_switch && doc != ctx.docs.active()) ctx.docs.set_active(doc);
                ImGui::EndTabItem();
            }
            if (!open) close_requests.push_back(doc);
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(bar_min.x, bar_min.y + bar_h));

    // Unmodified docs close right away; a modified one asks first.
    for (Document* doc : close_requests) {
        if (doc->modified()) {
            if (!s.pending_close) {
                s.pending_close = doc;
                s.open_close_popup = true;
            }
        } else {
            ctx.docs.close(doc);
            s.views.erase(doc);
        }
    }
    if (ctx.docs.documents().empty()) return;  // drawn as the empty state next frame

    Document* doc = ctx.docs.active();
    if (!doc) doc = ctx.docs.documents().front().get();
    View& view = s.view_for(doc);
    TextBuffer& buf = doc->buffer();
    if (doc != s.last_active) s.count_version = ~0ull;
    s.last_active = doc;
    if (window_focused) view.editor.focus();
    if (const int line = doc->take_reveal_request(); line > 0) view.editor.reveal_line(line);

    // ---- breadcrumb + lock ----
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 row_min = ImGui::GetCursorScreenPos();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float row_h = ImGui::GetTextLineHeight() + 10.0f;
        dl->AddRectFilled(row_min, ImVec2(row_min.x + row_w, row_min.y + row_h), theme::color(Color::CodeBg));
        dl->AddLine(ImVec2(row_min.x, row_min.y + row_h - 1.0f), ImVec2(row_min.x + row_w, row_min.y + row_h - 1.0f),
                    theme::color(Color::Separator));

        // Path segments relative to the project, else the OSEF checkout.
        std::vector<std::string> parts;
        fs::path rel;
        const fs::path project_dir = ctx.project ? canonical_or_self(ctx.project->directory()) : fs::path();
        rel = relative_inside(doc->path(), project_dir);
        if (!rel.empty()) {
            parts.push_back(project_dir.filename().string());
        } else {
            rel = relative_inside(doc->path(), canonical_or_self(ctx.engine_root));
        }
        if (rel.empty()) rel = doc->path();
        for (const auto& part : rel) {
            const std::string p = part.string();
            if (!p.empty() && p != "/") parts.push_back(p);
        }

        float x = row_min.x + 10.0f;
        const float ty = row_min.y + 5.0f;
        const std::string sep = " \xE2\x80\xBA ";  // ›
        for (size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            const ImU32 col = theme::color(last ? Color::Text : Color::TextDim);
            dl->AddText(ImVec2(x, ty), col, parts[i].c_str());
            x += ImGui::CalcTextSize(parts[i].c_str()).x;
            if (!last) {
                dl->AddText(ImVec2(x, ty), theme::color(Color::TextDisabled), sep.c_str());
                x += ImGui::CalcTextSize(sep.c_str()).x;
            }
        }

        // Lock toggle (right aligned). Engine sources get an explanatory label.
        const bool engine = !relative_inside(doc->path(), s.engine_dir).empty();
        const bool ro = doc->read_only();
        const char* label = ro ? (engine ? "Engine source (read-only) \xE2\x80\x94 click to unlock" : "Read-only \xE2\x80\x94 click to unlock")
                               : (engine ? "Engine source (editable) \xE2\x80\x94 click to lock" : nullptr);
        const float icon = ImGui::GetTextLineHeight() - 2.0f;
        const float label_w = label ? ImGui::CalcTextSize(label).x + 8.0f : 0.0f;
        const float btn_w = icon + 8.0f + label_w;
        const ImVec2 btn_min(row_min.x + row_w - btn_w - 8.0f, row_min.y + 2.0f);
        ImGui::SetCursorScreenPos(btn_min);
        const bool clicked = ImGui::InvisibleButton("##lock", ImVec2(btn_w, row_h - 4.0f));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            dl->AddRectFilled(btn_min, ImVec2(btn_min.x + btn_w, btn_min.y + row_h - 4.0f),
                              theme::color(ImGui::IsItemActive() ? Color::ButtonActive : Color::Hover), 3.0f);
            if (!label) ImGui::SetTooltip(ro ? "Read-only \xE2\x80\x94 click to unlock" : "Click to make read-only");
        }
        const ImU32 icol = ro ? theme::color(Color::Partial) : theme::color(hovered ? Color::Text : Color::TextDim);
        icons::draw(dl, ImVec2(btn_min.x + 4.0f, row_min.y + (row_h - icon) * 0.5f), icon,
                    ro ? icons::Glyph::Lock : icons::Glyph::Unlock, icol);
        if (label)
            dl->AddText(ImVec2(btn_min.x + icon + 10.0f, ty), theme::color(ro ? Color::TextDim : Color::Partial), label);
        if (clicked) {
            doc->set_read_only(!ro);
            view.editor.focus();
        }
        ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + row_h));
    }

    // ---- shortcuts (routed to this window and its children) ----
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) {
        s.find_open = true;
        s.find_focus = true;
        // Seed with a single-line selection, like most editors.
        if (buf.has_selection() && buf.selection_begin().line == buf.selection_end().line) {
            const std::string sel = buf.selected_text();
            if (!sel.empty() && sel.size() < sizeof(s.find_text)) std::snprintf(s.find_text, sizeof(s.find_text), "%s", sel.c_str());
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
        std::error_code ec;
        if (doc->modified() || !fs::exists(doc->path(), ec)) save_document(ctx, doc);
    }
    // Cmd/Ctrl+G and Shift+Cmd/Ctrl+G step through matches while the bar is open.
    for (const bool forward : {true, false}) {
        const ImGuiKeyChord chord = ImGuiMod_Ctrl | ImGuiKey_G | (forward ? 0 : ImGuiMod_Shift);
        if (s.find_open && s.find_text[0] && ImGui::Shortcut(chord, ImGuiInputFlags_Repeat)) {
            buf.find(s.find_text, forward, s.find_case);
            view.editor.scroll_to_cursor();
        }
    }

    // ---- find bar ----
    if (s.find_open) {
        const float bar_height = ImGui::GetFrameHeight() + 10.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::PanelBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
        ImGui::BeginChild("##findbar", ImVec2(0.0f, bar_height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        bool close = false;
        bool next = false, prev = false;
        icons::item(icons::Glyph::Search, ImGui::GetTextLineHeight(), theme::color(Color::TextDim));
        ImGui::SameLine();
        if (s.find_focus) {
            ImGui::SetKeyboardFocusHere();
            s.find_focus = false;
        }
        ImGui::SetNextItemWidth(260.0f);
        const ImGuiID input_id = ImGui::GetID("##find");
        const bool input_was_active = ImGui::GetActiveID() == input_id;
        // Esc closes the bar; keep the text instead of letting InputText revert it.
        const bool esc = input_was_active && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        std::string keep;
        if (esc) keep = s.find_text;
        const bool edited = ImGui::InputTextWithHint("##find", "Find", s.find_text, sizeof(s.find_text),
                                                     ImGuiInputTextFlags_CallbackAlways, find_input_callback,
                                                     &s.find_cursor_end);
        if (esc) {
            std::snprintf(s.find_text, sizeof(s.find_text), "%s", keep.c_str());
            close = true;
        }
        if (edited && s.find_text[0]) {
            // Incremental: search from the start of the current match.
            buf.set_cursor(buf.selection_begin());
            buf.find(s.find_text, true, s.find_case);
            view.editor.scroll_to_cursor();
        }
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            (ImGui::GetIO().KeyShift ? prev : next) = true;
            s.find_focus = true;  // Enter releases the input; keep typing there
            s.find_cursor_end = true;
        }
        if (input_was_active && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) next = true;

        ImGui::SameLine();
        const float sz = ImGui::GetTextLineHeight();
        {
            // "Aa" case toggle.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 tsz = ImGui::CalcTextSize("Aa");
            const ImVec2 bsz(tsz.x + 10.0f, ImGui::GetFrameHeight());
            if (ImGui::InvisibleButton("##case", bsz)) {
                s.find_case = !s.find_case;
                s.count_version = ~0ull;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const bool hov = ImGui::IsItemHovered();
            if (s.find_case || hov)
                dl->AddRectFilled(p, ImVec2(p.x + bsz.x, p.y + bsz.y),
                                  s.find_case ? (theme::color(Color::Selection)) : theme::color(Color::Hover), 3.0f);
            dl->AddText(ImVec2(p.x + 5.0f, p.y + (bsz.y - tsz.y) * 0.5f),
                        theme::color(s.find_case ? Color::Text : Color::TextDim), "Aa");
            if (hov) ImGui::SetTooltip("Match case");
        }
        ImGui::SameLine();

        // Match count, cached per (version, needle, case).
        if (s.count_version != buf.version() || s.count_needle != s.find_text || s.count_case != s.find_case) {
            s.count_version = buf.version();
            s.count_needle = s.find_text;
            s.count_case = s.find_case;
            s.matches = buf.find_all(s.count_needle, s.count_case);
        }
        int current = 0;
        if (buf.has_selection()) {
            const auto it = std::lower_bound(s.matches.begin(), s.matches.end(), buf.selection_begin());
            if (it != s.matches.end() && *it == buf.selection_begin() &&
                static_cast<size_t>(buf.selection_end().col - buf.selection_begin().col) == s.count_needle.size() &&
                buf.selection_end().line == buf.selection_begin().line)
                current = static_cast<int>(it - s.matches.begin()) + 1;
        }
        char count[64];
        if (!s.find_text[0])
            count[0] = '\0';
        else if (s.matches.empty())
            std::snprintf(count, sizeof(count), "No results");
        else if (current > 0)
            std::snprintf(count, sizeof(count), "%d of %zu", current, s.matches.size());
        else
            std::snprintf(count, sizeof(count), "%zu found", s.matches.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::color_vec(s.find_text[0] && s.matches.empty() ? Color::Error : Color::TextDim), "%-12s", count);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - sz - 6.0f) * 0.5f);
        // Up/Down arrows as small text buttons keep this independent of icon glyphs.
        if (ImGui::SmallButton("\xE2\x86\x91##prev")) prev = true;  // ↑
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous match (Shift+Enter)");
        ImGui::SameLine();
        if (ImGui::SmallButton("\xE2\x86\x93##next")) next = true;  // ↓
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next match (Enter)");
        ImGui::SameLine();
        if (icon_button("##close_find", icons::Glyph::Close, sz - 4.0f, theme::color(Color::TextDim), "Close (Esc)")) close = true;
        ImGui::EndChild();

        if ((next || prev) && s.find_text[0]) {
            buf.find(s.find_text, next, s.find_case);
            view.editor.scroll_to_cursor();
        }
        // Esc from the editor (or the panel) closes the bar too.
        if (!close && view.editor.focused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
        if (close) {
            s.find_open = false;
            view.editor.focus();
        }
    }
    view.editor.set_search_highlight(s.find_open ? s.find_text : "", s.find_case);

    // ---- editor ----
    ImGui::PushID(doc);
    view.editor.draw("##editor", buf, view.hl, s.style, ctx.fonts.mono, doc->read_only());
    ImGui::PopID();

    // ---- close confirmation ----
    if (s.open_close_popup) {
        ImGui::OpenPopup(kClosePopup);
        s.open_close_popup = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kClosePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        Document* target = s.pending_close;
        if (!target) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushFont(ctx.fonts.ui_bold, 0.0f);
            ImGui::Text("Save changes to %s?", target->title().c_str());
            ImGui::PopFont();
            ImGui::TextColored(theme::color_vec(Color::TextDim), "Your changes will be lost if you don't save them.");
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            const float w = 96.0f;
            bool done = false;
            if (ImGui::Button("Save", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                if (save_document(ctx, target)) {
                    ctx.docs.close(target);
                    s.views.erase(target);
                }
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(w, 0.0f))) {
                ctx.docs.close(target);
                s.views.erase(target);
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = true;
            if (done) {
                s.pending_close = nullptr;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace osef::editor
