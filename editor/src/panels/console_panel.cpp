// Unity-style Console: a toolbar (Clear, Collapse, search, per-level toggles
// with counts, running job + Cancel) above a clipped, selectable log list.

#include "panels/console_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace osef::editor {
namespace {

using theme::Color;

constexpr int kLevels = 4;

bool contains_nocase(std::string_view hay, std::string_view needle_lower) {
    if (needle_lower.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle_lower.begin(), needle_lower.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
    return it != hay.end();
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

icons::Glyph level_glyph(LogLevel level) {
    switch (level) {
    case LogLevel::Info: return icons::Glyph::Info;
    case LogLevel::Warning: return icons::Glyph::Warning;
    case LogLevel::Error: return icons::Glyph::Error;
    case LogLevel::Build: return icons::Glyph::Build;
    }
    return icons::Glyph::Info;
}

ImU32 level_color(LogLevel level) {
    switch (level) {
    case LogLevel::Info: return theme::color(Color::Info);
    case LogLevel::Warning: return theme::color(Color::Warning);
    case LogLevel::Error: return theme::color(Color::Error);
    case LogLevel::Build: return theme::color(Color::TextDim);
    }
    return theme::color(Color::Text);
}

// Compiler-style diagnostics inside build output get colored text.
ImU32 text_color(const LogLine& line) {
    if (line.level == LogLevel::Build) {
        if (line.text.find("error:") != std::string::npos || line.text.find("Error ") == 0 ||
            line.text.find("*** ") != std::string::npos)
            return theme::color(Color::Error);
        if (line.text.find("warning:") != std::string::npos) return theme::color(Color::Warning);
    }
    return theme::color(Color::Text);
}

// Finds "path:line" (as in "src/x.cpp:12:5: error: ...") whose file exists.
bool find_location(const EditorContext& ctx, const std::string& text, std::filesystem::path* file, int* line) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != ':' || !std::isdigit(static_cast<unsigned char>(text[i + 1]))) continue;
        size_t j = i + 1;
        int value = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) && value < 10000000)
            value = value * 10 + (text[j++] - '0');
        size_t start = i;
        while (start > 0 && !std::strchr(" \t\"'([<", text[start - 1])) --start;
        if (start == i || value <= 0) continue;
        const std::filesystem::path candidate(text.substr(start, i - start));
        std::vector<std::filesystem::path> tries;
        if (candidate.is_absolute()) {
            tries.push_back(candidate);
        } else {
            if (ctx.project) tries.push_back(ctx.project->directory() / candidate);
            if (!ctx.engine_root.empty()) tries.push_back(ctx.engine_root / candidate);
        }
        for (const auto& p : tries) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                *file = p;
                *line = value;
                return true;
            }
        }
    }
    return false;
}

void spinner(float size, ImU32 color) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    const float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(c, size * 0.38f, t, t + 4.4f, 20);
    dl->PathStroke(color, std::max(1.5f, size * 0.12f));
}

// Unity-style toolbar toggle: [icon count], pressed look when on.
bool level_toggle(const char* id, LogLevel level, size_t count, bool* on, const char* tooltip) {
    char label[24];
    if (count > 999)
        std::snprintf(label, sizeof(label), "999+");
    else
        std::snprintf(label, sizeof(label), "%zu", count);
    const float h = ImGui::GetFrameHeight();
    const float icon = std::floor(ImGui::GetFontSize() * 0.95f);
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float w = pad + icon + 4.0f + ImGui::CalcTextSize(label).x + pad;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *on = !*on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (*on)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::color(hovered ? Color::Hover : Color::ButtonActive),
                          rounding);
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::color(Color::Hover), rounding);
    if (*on) dl->AddRect(p, ImVec2(p.x + w, p.y + h), theme::color(Color::Border), rounding);
    ImU32 ic = level_color(level);
    if (level == LogLevel::Build) ic = theme::color(Color::Text);
    if (!*on) ic = (ic & ~IM_COL32_A_MASK) | (0x70u << IM_COL32_A_SHIFT);
    icons::draw(dl, ImVec2(p.x + pad, std::floor(p.y + (h - icon) * 0.5f)), icon, level_glyph(level), ic);
    dl->AddText(ImVec2(p.x + pad + icon + 4.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f),
                theme::color(*on ? Color::Text : Color::TextDim), label);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return clicked;
}

// Simple text toggle button (Collapse).
bool text_toggle(const char* label, bool* on) {
    if (*on) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::color(Color::ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color(Color::Hover));
    }
    const bool clicked = ImGui::Button(label);
    if (*on) ImGui::PopStyleColor(2);
    if (clicked) *on = !*on;
    return clicked;
}

} // namespace

struct ConsolePanel::State {
    // Local copy of the log (appended incrementally, trimmed with it).
    std::deque<LogLine> lines;
    uint64_t log_version = ~uint64_t{0};
    uint64_t last_serial = 0;

    // Visible rows after filtering/collapsing.
    struct Row {
        size_t line;  // index into `lines`
        int count;    // > 1 when collapsed duplicates
    };
    std::vector<Row> rows;
    bool rows_dirty = true;

    bool collapse = false;
    bool show[kLevels] = {true, true, true, true};
    char filter[256] = {};
    std::string filter_lower;

    // Selection: a range of rows, identified by their first line's serial.
    uint64_t anchor = 0;
    uint64_t cursor = 0;
    bool scroll_to_cursor = false;
    bool scroll_to_cursor_done = false;  // this frame: don't also stick to the bottom
    bool scroll_to_bottom = true;

    void sync(const Log& log) {
        const uint64_t v = log.version();
        if (v == log_version) return;
        log_version = v;
        uint64_t first = 0;
        size_t n = 0;
        log.visit([&](const LogLine& l) {
            if (n++ == 0) first = l.serial;
            if (l.serial > last_serial) lines.push_back(l);
        });
        if (n == 0) lines.clear();
        while (!lines.empty() && lines.front().serial < first) lines.pop_front();
        if (!lines.empty()) last_serial = lines.back().serial;
        rows_dirty = true;
    }

    void rebuild() {
        rows_dirty = false;
        rows.clear();
        std::unordered_map<std::string_view, size_t> seen[kLevels];
        for (size_t i = 0; i < lines.size(); ++i) {
            const LogLine& l = lines[i];
            const int lv = static_cast<int>(l.level);
            if (!show[lv] || !contains_nocase(l.text, filter_lower)) continue;
            if (collapse) {
                auto [it, inserted] = seen[lv].try_emplace(std::string_view(l.text), rows.size());
                if (!inserted) {
                    ++rows[it->second].count;
                    continue;
                }
            }
            rows.push_back(Row{i, 1});
        }
    }

    uint64_t serial_of(size_t row) const { return lines[rows[row].line].serial; }

    bool is_selected(size_t row) const {
        if (anchor == 0) return false;
        const uint64_t s = serial_of(row);
        return s >= std::min(anchor, cursor) && s <= std::max(anchor, cursor);
    }

    int row_of(uint64_t serial) const {
        for (size_t i = 0; i < rows.size(); ++i)
            if (serial_of(i) == serial) return static_cast<int>(i);
        return -1;
    }

    std::string selected_text() const {
        std::string out;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!is_selected(i)) continue;
            if (!out.empty()) out += '\n';
            out += lines[rows[i].line].text;
        }
        return out;
    }

    std::string all_text() const {
        std::string out;
        for (const Row& r : rows) {
            out += lines[r.line].text;
            out += '\n';
        }
        return out;
    }
};

ConsolePanel::ConsolePanel() : state_(new State) {}
ConsolePanel::~ConsolePanel() { delete state_; }

void ConsolePanel::draw(EditorContext& ctx) {
    State& st = *state_;
    st.sync(ctx.log);
    const ImGuiStyle& style = ImGui::GetStyle();

    // ---- Toolbar ----
    if (ImGui::Button("Clear")) {
        ctx.log.clear();
        st.anchor = st.cursor = 0;
        st.sync(ctx.log);
    }
    ImGui::SetItemTooltip("Remove all messages");
    ImGui::SameLine();
    if (text_toggle("Collapse", &st.collapse)) st.rows_dirty = true;
    ImGui::SetItemTooltip("Merge identical messages into one row with a count");
    ImGui::SameLine();

    // Right-aligned level toggles; measure them first so the search box fits.
    struct Toggle {
        const char* id;
        LogLevel level;
        const char* tip;
    };
    const Toggle toggles[] = {
        {"##info", LogLevel::Info, "Show info messages"},
        {"##warn", LogLevel::Warning, "Show warnings"},
        {"##error", LogLevel::Error, "Show errors"},
        {"##build", LogLevel::Build, "Show build/run output"},
    };
    float toggles_w = 0.0f;
    const float icon = std::floor(ImGui::GetFontSize() * 0.95f);
    for (const Toggle& t : toggles) {
        char label[24];
        const size_t count = ctx.log.count(t.level);
        if (count > 999)
            std::snprintf(label, sizeof(label), "999+");
        else
            std::snprintf(label, sizeof(label), "%zu", count);
        toggles_w += style.FramePadding.x * 2.0f + icon + 4.0f + ImGui::CalcTextSize(label).x;
    }
    toggles_w += style.ItemSpacing.x * 2.0f;  // spacing (the toggles themselves sit 2 px apart)

    const bool running = ctx.runner.running();
    float job_w = 0.0f;
    std::string job_text;
    if (running) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %.1f s", ctx.runner.elapsed_seconds());
        job_text = ctx.runner.current_label() + buf;
        job_w = ImGui::GetFontSize() + style.ItemSpacing.x + ImGui::CalcTextSize(job_text.c_str()).x +
                style.ItemSpacing.x + ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f +
                style.ItemSpacing.x;
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const float search_w = std::clamp(avail - toggles_w - job_w - style.ItemSpacing.x, 60.0f, 260.0f);
    ImGui::SetNextItemWidth(search_w);
    if (ImGui::InputTextWithHint("##search", "Search", st.filter, sizeof(st.filter))) {
        st.filter_lower = to_lower(st.filter);
        st.rows_dirty = true;
    }

    if (running) {
        ImGui::SameLine();
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f);
        spinner(ImGui::GetFontSize(), theme::color(Color::Accent));
        ImGui::SameLine();
        ImGui::SetCursorPosY(y);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(job_text.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ctx.runner.cancel();
        ImGui::SetItemTooltip("Stop the running job (SIGTERM, then SIGKILL after 2 s)");
    }

    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float x = std::max(ImGui::GetCursorPosX(), right - toggles_w + style.ItemSpacing.x * 2.0f);
    ImGui::SetCursorPosX(x);
    for (size_t i = 0; i < std::size(toggles); ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 2.0f);
        const int lv = static_cast<int>(toggles[i].level);
        if (level_toggle(toggles[i].id, toggles[i].level, ctx.log.count(toggles[i].level), &st.show[lv],
                         toggles[i].tip))
            st.rows_dirty = true;
    }

    if (st.rows_dirty) st.rebuild();

    // ---- List ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::PanelBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool list_open = ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                                             ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // Rows are exactly row_h apart (the clipper relies on it).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (list_open) {
        const bool focused = ImGui::IsWindowFocused();
        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        const float row_h = std::floor(ImGui::GetFontSize() + 6.0f);

        // Keyboard: arrows move the selection, Cmd/Ctrl+A selects all, Cmd/Ctrl+C copies.
        if (focused && !st.rows.empty()) {
            const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
            const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
            if (up || down) {
                const int last = static_cast<int>(st.rows.size()) - 1;
                const int cur = st.anchor ? st.row_of(st.cursor) : -1;
                const int next = cur < 0 ? (up ? last : 0) : std::clamp(cur + (up ? -1 : 1), 0, last);
                st.cursor = st.serial_of(static_cast<size_t>(next));
                if (!ImGui::GetIO().KeyShift || st.anchor == 0) st.anchor = st.cursor;
                st.scroll_to_cursor = true;
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) {
                st.anchor = st.serial_of(0);
                st.cursor = st.serial_of(st.rows.size() - 1);
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && st.anchor != 0)
                ImGui::SetClipboardText(st.selected_text().c_str());
        }
        if (st.scroll_to_cursor) {
            st.scroll_to_cursor = false;
            st.scroll_to_cursor_done = true;
            const int r = st.row_of(st.cursor);
            if (r >= 0) {
                const float top = static_cast<float>(r) * row_h;
                const float view = ImGui::GetWindowHeight();
                if (top < ImGui::GetScrollY())
                    ImGui::SetScrollY(top);
                else if (top + row_h > ImGui::GetScrollY() + view)
                    ImGui::SetScrollY(top + row_h - view);
            }
        }

        // Selection color dims when the console isn't focused (as in Unity).
        ImGui::PushStyleColor(ImGuiCol_Header,
                              theme::color(focused ? Color::Selection : Color::SelectionUnfocused));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                              theme::color(focused ? Color::Selection : Color::SelectionUnfocused));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::color(Color::Hover));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const float icon_size = std::floor(ImGui::GetFontSize() * 0.95f);
        ImFont* mono = ctx.fonts.mono;
        int open_menu_row = -1;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(st.rows.size()), row_h);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const State::Row& row = st.rows[static_cast<size_t>(i)];
                const LogLine& line = st.lines[row.line];
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (i % 2 == 1)
                    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + row_h), IM_COL32(255, 255, 255, 7));

                ImGui::PushID(i);
                const bool selected = st.is_selected(static_cast<size_t>(i));
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(width, row_h))) {
                    const uint64_t s = line.serial;
                    if (ImGui::GetIO().KeyShift && st.anchor != 0) {
                        st.cursor = s;
                    } else {
                        st.anchor = st.cursor = s;
                    }
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        std::filesystem::path file;
                        int ln = 0;
                        if (find_location(ctx, line.text, &file, &ln)) ctx.open_file(file, ln);
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    if (!selected) st.anchor = st.cursor = line.serial;
                    open_menu_row = i;
                }
                // Full text as a tooltip when the row is truncated or multi-line.
                const size_t nl = line.text.find('\n');
                const char* text_begin = line.text.c_str();
                const char* text_end = nl == std::string::npos ? text_begin + line.text.size() : text_begin + nl;
                ImGui::PopID();

                const float text_x = p.x + 6.0f + icon_size + 6.0f;
                icons::draw(dl, ImVec2(p.x + 6.0f, std::floor(p.y + (row_h - icon_size) * 0.5f)), icon_size,
                            level_glyph(line.level), level_color(line.level));
                const bool use_mono = line.level == LogLevel::Build && mono;
                ImFont* font = use_mono ? mono : ImGui::GetFont();
                const float font_size = use_mono ? mono->LegacySize * style.FontScaleDpi * style.FontScaleMain
                                                 : ImGui::GetFontSize();
                float right_edge = p.x + width - 6.0f;
                if (row.count > 1) {
                    char badge[16];
                    std::snprintf(badge, sizeof(badge), "%d", row.count > 9999 ? 9999 : row.count);
                    const ImVec2 bs = ImGui::CalcTextSize(badge);
                    const ImVec2 b0(right_edge - bs.x - 10.0f, p.y + (row_h - bs.y) * 0.5f - 1.0f);
                    const ImVec2 b1(right_edge, b0.y + bs.y + 2.0f);
                    dl->AddRectFilled(b0, b1, theme::color(Color::ButtonActive), (b1.y - b0.y) * 0.5f);
                    dl->AddText(ImVec2(b0.x + 5.0f, b0.y + 1.0f), theme::color(Color::Text), badge);
                    right_edge = b0.x - 6.0f;
                }
                const ImVec4 clip(text_x, p.y, right_edge, p.y + row_h);
                dl->AddText(font, font_size, ImVec2(text_x, std::floor(p.y + (row_h - font_size) * 0.5f)),
                            text_color(line), text_begin, text_end, 0.0f, &clip);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                    const float tw = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end).x;
                    if (nl != std::string::npos || text_x + tw > right_edge) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45.0f);
                        ImGui::TextUnformatted(line.text.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
            }
        }
        clipper.End();
        ImGui::PopStyleColor(3);

        if (st.rows.empty()) {
            const char* hint = st.lines.empty() ? "No messages" : "No messages match the filters";
            ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
            ImGui::TextDisabled("%s", hint);
        }

        if (open_menu_row >= 0) ImGui::OpenPopup("##console_menu");
        if (ImGui::BeginPopup("##console_menu")) {
            if (ImGui::MenuItem("Copy", ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+C" : "Ctrl+C", false,
                                st.anchor != 0))
                ImGui::SetClipboardText(st.selected_text().c_str());
            if (ImGui::MenuItem("Copy All")) ImGui::SetClipboardText(st.all_text().c_str());
            const int r = st.row_of(st.cursor);
            std::filesystem::path file;
            int ln = 0;
            const bool has_loc =
                r >= 0 && find_location(ctx, st.lines[st.rows[static_cast<size_t>(r)].line].text, &file, &ln);
            if (ImGui::MenuItem("Open Location", nullptr, false, has_loc)) ctx.open_file(file, ln);
            ImGui::Separator();
            if (ImGui::MenuItem("Clear")) {
                ctx.log.clear();
                st.anchor = st.cursor = 0;
            }
            ImGui::EndPopup();
        }

        // Follow new output while the view is scrolled to the bottom.
        if (st.scroll_to_bottom || (at_bottom && !st.scroll_to_cursor_done)) ImGui::SetScrollHereY(1.0f);
        st.scroll_to_bottom = false;
        st.scroll_to_cursor_done = false;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

} // namespace osef::editor
