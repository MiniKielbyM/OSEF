// Hierarchy: the OS as a tree of components, drawn as custom full-width rows
// (Unity style) with search, keyboard navigation and a context menu.

#include "panels/hierarchy_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"
#include "model/component.h"
#include "model/project.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string_view>

namespace osef::editor {

using theme::Color;

namespace {

constexpr float kMargin = 6.0f;   // toolbar margin
constexpr float kIndent = 14.0f;  // per tree level
constexpr float kArrowW = 13.0f;
constexpr float kIconSize = 16.0f;
constexpr int kScrollFrames = 3;  // see TreeState::scroll_to_selection

ImU32 col(Color c) { return theme::color(c); }

ImU32 with_alpha(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}


std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool is_disabled(const Component& c) { return c.toggleable && !c.enabled; }

void open_primary_source(EditorContext& ctx, const Component& c) {
    if (!ctx.project || c.sources.empty()) return;
    const SourceRef& ref = c.sources.front();
    ctx.open_file(ctx.project->resolve(ref), ref.line);
    ctx.request_focus_code = true;
}

void create_driver_stub(EditorContext& ctx, const Component& c) {
    if (!ctx.project) return;
    std::string error;
    const std::filesystem::path path = ctx.project->create_driver_stub(c, &error);
    if (path.empty()) {
        ctx.log.error("Create Driver Stub for " + c.name + " failed: " + (error.empty() ? "unknown error" : error));
        ctx.request_focus_console = true;
        return;
    }
    ctx.log.info("Created driver stub for " + c.name + ": " + path.string());
    ctx.open_file(path);
    ctx.request_focus_code = true;
}

void set_enabled(EditorContext& ctx, Component& c, bool enabled) {
    if (!ctx.project || !c.toggleable || c.enabled == enabled) return;
    c.enabled = enabled;
    ctx.project->apply_enabled(c);
}

struct TreeState {
    struct Row {
        Component* c = nullptr;
        int depth = 0;
        bool has_children = false;
        bool open = false;
    };

    std::unordered_map<std::string, bool> open;  // expand state per component id
    char search[128] = {};
    std::string last_selection;
    // Frames left to keep the selected row in view. A request lasts a few
    // frames because the dock layout (and so the window height) settles late.
    int scroll_to_selection = 0;
    std::string context_id;                      // row whose context menu is open
    std::vector<Row> rows;                       // visible rows, top to bottom

    // Search: ids of components that match or have a matching descendant.
    std::string query;
    std::unordered_set<const Component*> visible;
    std::unordered_set<const Component*> matched;

    bool filtering() const { return !query.empty(); }

    bool is_open(const Component& c) {
        auto it = open.find(c.id);
        if (it != open.end()) return it->second;
        const bool def = c.kind == ComponentKind::Os || c.id == "boot" || c.id == "hw" || c.id == "kernel" ||
                         c.id == "user";
        open.emplace(c.id, def);
        return def;
    }

    void set_open_recursive(Component& c, bool value) {
        if (!c.children.empty()) open[c.id] = value;
        for (auto& child : c.children) set_open_recursive(*child, value);
    }

    bool update_filter(const Component& c) {
        bool any = false;
        for (const auto& child : c.children) any |= update_filter(*child);
        const bool self = lower(c.name).find(query) != std::string::npos ||
                          lower(c.id).find(query) != std::string::npos;
        if (self) matched.insert(&c);
        if (self || any) visible.insert(&c);
        return self || any;
    }

    void flatten(Component& c, int depth) {
        if (filtering() && !visible.count(&c)) return;
        Row row;
        row.c = &c;
        row.depth = depth;
        row.has_children = !c.children.empty();
        row.open = row.has_children && (filtering() || is_open(c));
        rows.push_back(row);
        if (row.open)
            for (auto& child : c.children) flatten(*child, depth + 1);
    }

    void rebuild(Component& root) {
        query = lower(search);
        visible.clear();
        matched.clear();
        if (filtering()) update_filter(root);
        rows.clear();
        flatten(root, 0);
    }

    int index_of(const std::string& id) const {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].c->id == id) return static_cast<int>(i);
        return -1;
    }
};

} // namespace

struct HierarchyPanel::State : TreeState {};

HierarchyPanel::HierarchyPanel() : state_(new State()) {}
HierarchyPanel::~HierarchyPanel() { delete state_; }

namespace {

// "+" dropdown and search field.
void draw_toolbar(EditorContext& ctx, TreeState& st) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(start.x + kMargin, start.y + kMargin - 1.0f));

    // [+ v] button.
    const ImVec2 add_size(std::round(frame_h * 1.65f), frame_h);
    if (ImGui::Button("##add", add_size)) ImGui::OpenPopup("##add_menu");
    ImGui::SetItemTooltip("Add hardware, driver stubs, expand or collapse");
    {
        const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float g = 12.0f;
        icons::draw(dl, ImVec2(r.Min.x + 5.0f, std::floor(r.GetCenter().y - g * 0.5f)), g, icons::Glyph::Plus,
                    col(Color::Text));
        const float cx = std::floor(r.Max.x - 8.0f), cy = std::floor(r.GetCenter().y) + 0.5f;
        dl->AddTriangleFilled(ImVec2(cx - 3.0f, cy - 1.5f), ImVec2(cx + 3.0f, cy - 1.5f), ImVec2(cx, cy + 2.0f),
                              col(Color::Text));
    }
    if (ImGui::BeginPopup("##add_menu")) {
        Project& project = *ctx.project;
        if (ImGui::MenuItem("Add Hardware Declaration", nullptr, false, !project.hardware().found))
            project.add_hardware_declaration();
        if (project.hardware().found) ImGui::SetItemTooltip("main.cpp already has an OSEF_HARDWARE declaration");
        Component* sel = ctx.selected();
        const bool planned = sel && sel->status == ComponentStatus::Planned;
        const std::string stub_label =
            planned ? "Create Driver Stub for " + sel->name + "\xE2\x80\xA6" : std::string("Create Driver Stub\xE2\x80\xA6");
        if (ImGui::MenuItem(stub_label.c_str(), nullptr, false, planned)) create_driver_stub(ctx, *sel);
        if (!planned) ImGui::SetItemTooltip("Select a planned component first");
        ImGui::Separator();
        if (ImGui::MenuItem("Expand All")) st.set_open_recursive(project.root(), true);
        if (ImGui::MenuItem("Collapse All")) {
            st.set_open_recursive(project.root(), false);
            st.open[project.root().id] = true;
        }
        ImGui::EndPopup();
    }

    // Search field with a magnifier placeholder and a clear button.
    ImGui::SameLine(0.0f, 4.0f);
    const float avail = ImGui::GetContentRegionAvail().x - kMargin;
    const float icon = 13.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(icon + 9.0f, style.FramePadding.y));
    ImGui::SetNextItemWidth(std::max(40.0f, avail));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InputTextWithHint("##search", "Search", st.search, sizeof(st.search));
    ImGui::PopStyleVar();
    const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) st.search[0] = '\0';
    ImDrawList* dl = ImGui::GetWindowDrawList();
    icons::draw(dl, ImVec2(r.Min.x + 6.0f, std::floor(r.GetCenter().y - icon * 0.5f)), icon, icons::Glyph::Search,
                col(Color::TextDim));
    if (st.search[0]) {
        const float bs = r.GetHeight();
        ImGui::SetCursorScreenPos(ImVec2(r.Max.x - bs, r.Min.y));
        if (ImGui::InvisibleButton("##clear", ImVec2(bs, bs))) st.search[0] = '\0';
        const float g = 9.0f;
        icons::draw(dl, ImVec2(std::floor(r.Max.x - bs * 0.5f - g * 0.5f), std::floor(r.GetCenter().y - g * 0.5f)), g,
                    icons::Glyph::Close, col(ImGui::IsItemHovered() ? Color::Text : Color::TextDim));
    }

    // Bottom border under the toolbar, full width.
    const ImVec2 wp = ImGui::GetWindowPos();
    const float y = std::floor(r.Max.y + kMargin - 1.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y + 0.5f), ImVec2(wp.x + ImGui::GetWindowWidth(), y + 0.5f),
                                        col(Color::Separator));
    ImGui::SetCursorScreenPos(ImVec2(wp.x, y + 1.0f));
}

// Keyboard navigation (panel focused, nothing being edited).
void handle_keys(EditorContext& ctx, TreeState& st) {
    if (st.rows.empty() || ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput) return;
    int idx = st.index_of(ctx.selection);
    auto select_row = [&](int i) {
        i = std::clamp(i, 0, static_cast<int>(st.rows.size()) - 1);
        if (st.rows[static_cast<size_t>(i)].c->id != ctx.selection) {
            ctx.select(st.rows[static_cast<size_t>(i)].c->id);
            st.scroll_to_selection = kScrollFrames;
        }
    };
    const bool repeat = true;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, repeat)) {
        select_row(idx < 0 ? 0 : idx + 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, repeat)) {
        select_row(idx < 0 ? 0 : idx - 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        select_row(0);
    } else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
        select_row(static_cast<int>(st.rows.size()) - 1);
    } else if (idx >= 0 && ImGui::IsKeyPressed(ImGuiKey_RightArrow, repeat)) {
        const TreeState::Row& row = st.rows[static_cast<size_t>(idx)];
        if (row.has_children && !row.open) st.open[row.c->id] = true;
        else if (row.open) select_row(idx + 1);
    } else if (idx >= 0 && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, repeat)) {
        const TreeState::Row& row = st.rows[static_cast<size_t>(idx)];
        if (row.open && !st.filtering()) {
            st.open[row.c->id] = false;
        } else if (row.c->parent) {
            ctx.select(row.c->parent->id);
            st.scroll_to_selection = kScrollFrames;
        }
    } else if (idx >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        open_primary_source(ctx, *st.rows[static_cast<size_t>(idx)].c);
    }
}

void draw_tag(ImDrawList* dl, ImFont* font, float font_size, float right, float cy, const char* text, ImU32 text_col,
              ImU32 bg) {
    const ImVec2 ts = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    const float pad_x = 5.0f, h = std::round(ts.y + 2.0f);
    const ImVec2 min(std::floor(right - ts.x - pad_x * 2.0f), std::floor(cy - h * 0.5f));
    const ImVec2 max(std::floor(right), min.y + h);
    dl->AddRectFilled(min, max, bg, h * 0.5f);
    dl->AddText(font, font_size, ImVec2(min.x + pad_x, min.y + std::floor((h - ts.y) * 0.5f)), text_col, text);
}

} // namespace

void HierarchyPanel::draw(EditorContext& ctx) {
    State& st = *state_;
    if (!ctx.project) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + kMargin * 2.0f, start.y + kMargin * 2.0f));
        ImGui::TextColored(theme::color_vec(Color::TextDim), "No project open");
        return;
    }
    Component& root = ctx.project->root();
    const bool panel_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    draw_toolbar(ctx, st);

    // External selection change (Inspector breadcrumb, --select, ...): reveal it.
    if (ctx.selection != st.last_selection) {
        st.last_selection = ctx.selection;
        if (Component* sel = ctx.selected()) {
            for (Component* p = sel->parent; p; p = p->parent) st.open[p->id] = true;
            // Also reveal its children (e.g. --select user.programs shows the programs).
            if (!sel->children.empty()) st.open[sel->id] = true;
            st.scroll_to_selection = kScrollFrames;
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool child_open = ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                              ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    if (child_open) {
        st.rebuild(root);
        if (panel_focused) {
            handle_keys(ctx, st);
            st.rebuild(root);
            st.last_selection = ctx.selection;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float font_size = ImGui::GetFontSize();
        ImFont* bold = ctx.fonts.ui_bold ? ctx.fonts.ui_bold : font;
        ImFont* small = ctx.fonts.small ? ctx.fonts.small : font;
        const float small_size = ctx.fonts.small && ctx.fonts.ui && ctx.fonts.ui->LegacySize > 0.0f
                                     ? std::round(font_size * ctx.fonts.small->LegacySize / ctx.fonts.ui->LegacySize)
                                     : std::round(font_size * 0.8f);
        const float row_h = std::round(font_size + 5.0f);
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        const float win_top = ImGui::GetWindowPos().y, win_bottom = win_top + ImGui::GetWindowHeight();

        ImGui::Dummy(ImVec2(width, 2.0f));  // a little air under the toolbar
        std::string clicked_id, toggled_id, double_clicked_id;
        bool toggle_recursive = false;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(st.rows.size()), row_h);
        const int sel_idx = st.index_of(ctx.selection);
        if (st.scroll_to_selection > 0 && sel_idx >= 0) clipper.IncludeItemByIndex(sel_idx);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const TreeState::Row& row = st.rows[static_cast<size_t>(i)];
                Component& c = *row.c;
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(c.id.c_str());
                ImGui::InvisibleButton("##row", ImVec2(width, row_h),
                                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                ImGui::PopID();
                const bool hovered = ImGui::IsItemHovered();
                const bool selected = c.id == ctx.selection;
                const float x_arrow = pos.x + 4.0f + kIndent * static_cast<float>(row.depth);
                const float mouse_x = ImGui::GetIO().MousePos.x;
                const bool on_arrow = row.has_children && mouse_x >= x_arrow - 2.0f && mouse_x < x_arrow + kArrowW;

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    if (on_arrow) {
                        toggled_id = c.id;
                        toggle_recursive = ImGui::GetIO().KeyAlt;
                    } else {
                        clicked_id = c.id;
                    }
                }
                if (hovered && !on_arrow && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (row.has_children && c.sources.empty()) toggled_id = c.id;
                    else double_clicked_id = c.id;
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    st.context_id = c.id;
                    ctx.selection = c.id;  // select without opening the source (Unity selects on right-click)
                    st.last_selection = c.id;
                    ImGui::OpenPopup("##row_menu");
                }
                if (selected && st.scroll_to_selection > 0) {
                    if (pos.y < win_top || pos.y + row_h > win_bottom) ImGui::SetScrollHereY(0.5f);
                    --st.scroll_to_selection;
                }

                // Background.
                const ImVec2 min(pos.x, pos.y), max(pos.x + width, pos.y + row_h);
                if (selected) dl->AddRectFilled(min, max, col(panel_focused ? Color::Selection : Color::SelectionUnfocused));
                else if (hovered) dl->AddRectFilled(min, max, col(Color::Hover));

                const bool planned = c.status == ComponentStatus::Planned;
                const bool disabled = is_disabled(c);
                const bool context_only = st.filtering() && !st.matched.count(&c);
                const float cy = std::floor(pos.y + row_h * 0.5f);

                // Expand arrow.
                if (row.has_children) {
                    const ImU32 ac = col(hovered && on_arrow ? Color::Text : Color::TextDim);
                    const float ax = std::floor(x_arrow + kArrowW * 0.5f) - 0.5f, ay = cy + 0.5f;
                    if (row.open)
                        dl->AddTriangleFilled(ImVec2(ax - 4.0f, ay - 2.0f), ImVec2(ax + 4.0f, ay - 2.0f),
                                              ImVec2(ax, ay + 2.5f), ac);
                    else
                        dl->AddTriangleFilled(ImVec2(ax - 2.0f, ay - 4.0f), ImVec2(ax + 2.5f, ay),
                                              ImVec2(ax - 2.0f, ay + 4.0f), ac);
                }

                // Icon.
                const float x_icon = x_arrow + kArrowW;
                float icon_alpha = 1.0f;
                if (planned) icon_alpha = 0.5f;
                if (disabled || context_only) icon_alpha = 0.4f;
                icons::draw(dl, ImVec2(x_icon, cy - kIconSize * 0.5f), kIconSize, c.kind,
                            with_alpha(icons::kind_color(c.kind), icon_alpha));

                // Tag, then the name clipped before it.
                float name_right = max.x - 6.0f;
                const char* tag = planned ? "planned" : c.status == ComponentStatus::Partial ? "partial" : nullptr;
                if (tag) {
                    const ImU32 tag_col = planned ? col(Color::TextDim) : col(Color::Partial);
                    const ImVec2 ts = small->CalcTextSizeA(small_size, FLT_MAX, 0.0f, tag);
                    const float tag_w = ts.x + 10.0f;
                    if (width - (x_icon - pos.x) > tag_w + 70.0f) {
                        draw_tag(dl, small, small_size, max.x - 6.0f, cy + 0.5f, tag, tag_col,
                                 selected ? IM_COL32(0, 0, 0, 0x40) : IM_COL32(0, 0, 0, 0x30));
                        name_right = max.x - 6.0f - tag_w - 6.0f;
                    }
                }
                const bool is_root = c.kind == ComponentKind::Os;
                ImFont* name_font = is_root ? bold : font;
                ImU32 name_col = col(Color::Text);
                if (planned || context_only) name_col = col(Color::TextDim);
                if (disabled) name_col = col(selected ? Color::TextDim : Color::TextDisabled);
                if (selected && panel_focused && !disabled) name_col = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
                const float x_text = x_icon + kIconSize + 5.0f;
                const ImVec2 text_pos(x_text, std::floor(cy - font_size * 0.5f));
                const ImVec2 ts = name_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, c.name.c_str());
                dl->PushClipRect(ImVec2(x_text, min.y), ImVec2(std::max(x_text, name_right), max.y), true);
                dl->AddText(name_font, font_size, text_pos, name_col, c.name.c_str());
                if (disabled) {
                    const float sy = std::floor(text_pos.y + font_size * 0.55f) + 0.5f;
                    dl->AddLine(ImVec2(x_text - 1.0f, sy), ImVec2(x_text + ts.x + 1.0f, sy), name_col, 1.0f);
                }
                dl->PopClipRect();

                if (hovered && !on_arrow) {
                    std::string tip;
                    if (!c.summary.empty()) tip = c.summary;
                    if (disabled) tip += (tip.empty() ? "" : "\n") + std::string("Disabled in OSEF_HARDWARE");
                    if (planned) tip += (tip.empty() ? "" : "\n") + std::string("Planned: not implemented in the engine yet");
                    if (!tip.empty()) ImGui::SetItemTooltip("%s", tip.c_str());
                }
            }
        }
        clipper.End();
        if (sel_idx < 0) st.scroll_to_selection = 0;
        ImGui::Dummy(ImVec2(width, 4.0f));

        // Clicking empty space below the rows clears the selection (like Unity).
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ctx.selection.clear();

        // Apply interactions after drawing so the row list stays consistent this frame.
        if (!toggled_id.empty()) {
            if (Component* c = ctx.project->find(toggled_id)) {
                const bool now = !st.is_open(*c);
                if (toggle_recursive) st.set_open_recursive(*c, now);
                else st.open[c->id] = now;
            }
        }
        if (!clicked_id.empty() && clicked_id != ctx.selection) {
            ctx.select(clicked_id);
            st.last_selection = ctx.selection;
        }
        if (!double_clicked_id.empty())
            if (Component* c = ctx.project->find(double_clicked_id)) open_primary_source(ctx, *c);

        // Row context menu.
        if (ImGui::BeginPopup("##row_menu")) {
            Component* c = ctx.project->find(st.context_id);
            if (!c) {
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::TextDisabled("%s", c->name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Open Source", nullptr, false, !c->sources.empty())) open_primary_source(ctx, *c);
                if (c->toggleable) {
                    if (ImGui::MenuItem(c->enabled ? "Disable" : "Enable")) set_enabled(ctx, *c, !c->enabled);
                    if (!c->toggle_tooltip.empty()) ImGui::SetItemTooltip("%s", c->toggle_tooltip.c_str());
                }
                if (c->status == ComponentStatus::Planned &&
                    ImGui::MenuItem("Create Driver Stub\xE2\x80\xA6"))
                    create_driver_stub(ctx, *c);
                if (!c->children.empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Expand All")) st.set_open_recursive(*c, true);
                    if (ImGui::MenuItem("Collapse All")) st.set_open_recursive(*c, false);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy ID")) ImGui::SetClipboardText(c->id.c_str());
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace osef::editor
