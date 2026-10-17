// CodeEditor: virtualized rendering of a TextBuffer plus mouse/keyboard input.
//
// Layout inside the child window (WindowPadding 0, content size set
// explicitly so the scrollbars match the document):
//
//   | gutter (line numbers, fixed) | pad | text (scrolls in x and y) |
//
// Text positions are mapped through display columns (tabs expanded), and
// each display column is one monospace cell `cw` wide.

#include "widgets/code_editor.h"

#include "app/theme.h"
#include "text/cpp_highlighter.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>
#include <utility>

namespace osef::editor {

namespace {

using theme::Color;

ImU32 token_color(TokenKind kind) {
    switch (kind) {
        case TokenKind::Keyword: return theme::color(Color::SynKeyword);
        case TokenKind::Control: return theme::color(Color::SynControl);
        case TokenKind::Type: return theme::color(Color::SynType);
        case TokenKind::Function: return theme::color(Color::SynFunction);
        case TokenKind::String: return theme::color(Color::SynString);
        case TokenKind::Number: return theme::color(Color::SynNumber);
        case TokenKind::Comment: return theme::color(Color::SynComment);
        case TokenKind::Preprocessor: return theme::color(Color::SynPreprocessor);
        case TokenKind::Macro: return theme::color(Color::SynMacro);
        case TokenKind::Punctuation: return theme::color(Color::SynPunctuation);
        case TokenKind::Text: break;
    }
    return theme::color(Color::SynIdentifier);
}

ImU32 scale_alpha(ImU32 c, float factor) {
    const ImU32 a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 na = static_cast<ImU32>(std::clamp(static_cast<float>(a) * factor, 0.0f, 255.0f));
    return (c & ~IM_COL32_A_MASK) | (na << IM_COL32_A_SHIFT);
}

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
bool is_blank(char c) { return c == ' ' || c == '\t'; }

int digit_count(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        ++d;
    }
    return d;
}

// Appends code point `c` as UTF-8.
void append_utf8(std::string& out, unsigned int c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

size_t find_in(std::string_view hay, std::string_view needle, size_t from, bool case_sensitive) {
    if (needle.empty() || needle.size() > hay.size()) return std::string_view::npos;
    if (case_sensitive) return hay.find(needle, from);
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && ascii_lower(hay[i + k]) == ascii_lower(needle[k])) ++k;
        if (k == needle.size()) return i;
    }
    return std::string_view::npos;
}

// Display column of a line's indentation, or -1 for a blank line.
int indent_of(const TextBuffer& buf, int line) {
    const std::string& s = buf.line(line);
    int i = 0;
    while (i < static_cast<int>(s.size()) && is_blank(s[i])) ++i;
    if (i == static_cast<int>(s.size())) return -1;
    return buf.visual_column(line, i);
}

struct Geometry {
    ImVec2 view_min, view_max;  // visible text + gutter area (screen space, scrollbars excluded)
    float gutter_w = 0.0f;
    float text_x0 = 0.0f;       // screen x of display column 0 at scroll 0
    float line_h = 0.0f;
    float cw = 0.0f;
    float scroll_x = 0.0f, scroll_y = 0.0f;

    float x_of(int vcol) const { return text_x0 + static_cast<float>(vcol) * cw - scroll_x; }
    float y_of(int line) const { return view_min.y + static_cast<float>(line) * line_h - scroll_y; }
    float view_w() const { return view_max.x - view_min.x; }
    float view_h() const { return view_max.y - view_min.y; }
    float text_view_w() const { return view_max.x - (view_min.x + gutter_w); }
};

TextPos hit_test(const TextBuffer& buf, const Geometry& g, ImVec2 mouse) {
    const float fl = std::floor((mouse.y - g.view_min.y + g.scroll_y) / g.line_h);
    if (fl < 0.0f) return {0, 0};
    const int line = static_cast<int>(fl);
    if (line >= buf.line_count()) {
        const int last = buf.line_count() - 1;
        return {last, static_cast<int>(buf.line(last).size())};
    }
    const float vcol = (mouse.x - g.text_x0 + g.scroll_x) / g.cw;
    return {line, buf.column_at_visual(line, std::max(0.0f, vcol))};
}

} // namespace

void CodeEditor::reveal_line(int line) {
    if (line > 0) reveal_line_ = line;
}

void CodeEditor::set_search_highlight(std::string needle, bool case_sensitive) {
    search_ = std::move(needle);
    search_case_ = case_sensitive;
}

int CodeEditor::max_columns(const TextBuffer& buf) {
    if (cols_version_ != buf.version()) {
        cols_version_ = buf.version();
        max_cols_ = 0;
        for (int l = 0; l < buf.line_count(); ++l)
            max_cols_ = std::max(max_cols_, buf.visual_column(l, static_cast<int>(buf.line(l).size())));
    }
    return max_cols_;
}

bool CodeEditor::handle_keyboard(TextBuffer& buf, bool read_only, int page_lines, int* scroll_lines) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool shift = io.KeyShift;
    const bool ctrl = io.KeyCtrl;  // Cmd on macOS
    const bool alt = io.KeyAlt;
    const bool word = mac ? alt : ctrl;
    const uint64_t before = buf.version();
    auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };

    // ---- navigation (always allowed) ----
    if (pressed(ImGuiKey_LeftArrow)) {
        if (mac && ctrl)
            buf.move_home(shift);
        else
            buf.move_left(shift, word);
    }
    if (pressed(ImGuiKey_RightArrow)) {
        if (mac && ctrl)
            buf.move_end(shift);
        else
            buf.move_right(shift, word);
    }
    if (pressed(ImGuiKey_UpArrow)) {
        if (mac && ctrl)
            buf.move_doc_start(shift);
        else
            buf.move_up(shift);
    }
    if (pressed(ImGuiKey_DownArrow)) {
        if (mac && ctrl)
            buf.move_doc_end(shift);
        else
            buf.move_down(shift);
    }
    if (pressed(ImGuiKey_Home)) {
        if (ctrl)
            buf.move_doc_start(shift);
        else
            buf.move_home(shift);
    }
    if (pressed(ImGuiKey_End)) {
        if (ctrl)
            buf.move_doc_end(shift);
        else
            buf.move_end(shift);
    }
    if (pressed(ImGuiKey_PageUp)) {
        buf.move_lines(-page_lines, shift);
        *scroll_lines -= page_lines;
    }
    if (pressed(ImGuiKey_PageDown)) {
        buf.move_lines(page_lines, shift);
        *scroll_lines += page_lines;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && buf.has_selection()) buf.clear_selection();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) buf.select_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
        // Without a selection, copy the whole line (like VS Code).
        const std::string text = buf.has_selection() ? buf.selected_text() : buf.line(buf.cursor().line) + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    if (read_only) return buf.version() != before;

    // ---- editing ----
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) {
        if (buf.has_selection()) {
            ImGui::SetClipboardText(buf.selected_text().c_str());
            buf.backspace();
        } else {
            ImGui::SetClipboardText((buf.line(buf.cursor().line) + "\n").c_str());
            buf.delete_line();
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_Repeat)) {
        if (const char* clip = ImGui::GetClipboardText(); clip && *clip) {
            buf.break_undo_group();
            buf.insert(clip);
            buf.break_undo_group();
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_Repeat)) buf.undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_Repeat) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_Repeat))
        buf.redo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Slash, ImGuiInputFlags_Repeat)) buf.toggle_line_comment();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K, ImGuiInputFlags_Repeat)) buf.delete_line();

    if (pressed(ImGuiKey_Tab) && !ctrl && !alt) {
        if (shift)
            buf.unindent();
        else
            buf.indent();
    }
    if ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) && !ctrl && !alt) buf.newline();
    if (pressed(ImGuiKey_Backspace)) {
        if (!buf.has_selection()) {
            if (mac && ctrl)
                buf.set_cursor({buf.cursor().line, 0}, true);  // Cmd+Backspace: to line start
            else if (word)
                buf.move_left(true, true);
        }
        buf.backspace();
    }
    if (pressed(ImGuiKey_Delete)) {
        if (!buf.has_selection() && word) buf.move_right(true, true);
        buf.delete_forward();
    }
    return buf.version() != before;
}

void CodeEditor::handle_text_input(TextBuffer& buf) {
    ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    // Shortcut chords never type; AltGr (Ctrl+Alt) does on Windows/Linux layouts.
    const bool blocked = mac ? (io.KeyCtrl || io.KeySuper) : (io.KeyCtrl && !io.KeyAlt);
    if (!blocked) {
        std::string text;
        for (ImWchar c : io.InputQueueCharacters) {
            const unsigned int cp = static_cast<unsigned int>(c);
            if (cp < 32 || cp == 127) continue;
            if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // macOS function-key private range
            append_utf8(text, cp);
        }
        // One insert per character so typing coalesces like keystrokes do.
        for (size_t i = 0; i < text.size();) {
            size_t n = 1;
            while (i + n < text.size() && is_cont(static_cast<unsigned char>(text[i + n]))) ++n;
            buf.insert(std::string_view(text).substr(i, n));
            i += n;
        }
    }
    io.InputQueueCharacters.resize(0);
}

bool CodeEditor::draw(const char* id, TextBuffer& buf, CppHighlighter& hl, const CodeEditorStyle& style,
                      ImFont* mono, bool read_only, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    const uint64_t version_before = buf.version();

    // Metrics come from the mono font at the code size; drawing uses the
    // font explicitly so popups keep the UI font.
    ImGui::PushFont(mono, style.font_size > 0.0f ? style.font_size : (mono ? mono->LegacySize : 0.0f));
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float cw = std::max(1.0f, ImGui::GetFontBaked()->GetCharAdvance(static_cast<ImWchar>(' ')));
    ImGui::PopFont();

    hl.update(buf);

    Geometry g;
    g.cw = cw;
    g.line_h = std::floor(fs * style.line_spacing + 0.5f);
    const int digits = std::max(3, digit_count(buf.line_count()));
    const float gutter_pad_l = std::floor(cw * 1.5f), gutter_pad_r = std::floor(cw * 2.0f);
    g.gutter_w = style.line_numbers ? std::floor(gutter_pad_l + cw * static_cast<float>(digits) + gutter_pad_r)
                                    : std::floor(cw);
    const float text_pad = std::floor(cw * 0.5f);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x > 0.0f) avail.x = size.x;
    if (size.y > 0.0f) avail.y = size.y;
    avail.x = std::max(avail.x, 1.0f);
    avail.y = std::max(avail.y, 1.0f);

    auto content_height = [&](float view_h) {
        return static_cast<float>(buf.line_count()) * g.line_h + std::floor(view_h * style.scroll_past_end);
    };
    const float content_w = g.gutter_w + text_pad + static_cast<float>(max_columns(buf) + 8) * cw;
    ImGui::SetNextWindowContentSize(ImVec2(content_w, content_height(avail.y)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::CodeBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs;
    const bool open = ImGui::BeginChild(id, avail, ImGuiChildFlags_None, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::EndChild();
        return false;
    }

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (want_focus_) {
        ImGui::SetWindowFocus();
        want_focus_ = false;
    }
    bool focused = ImGui::IsWindowFocused();
    // ImGui's nav moves focus to the parent window on Escape; keep it here.
    if (!focused && focused_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        ImGui::SetWindowFocus();
        focused = true;
    }
    focused_ = focused;

    g.view_min = win->InnerRect.Min;
    g.view_max = win->InnerRect.Max;
    g.text_x0 = g.view_min.x + g.gutter_w + text_pad;
    g.scroll_x = win->Scroll.x;
    g.scroll_y = win->Scroll.y;
    const int page_lines = std::max(1, static_cast<int>(g.view_h() / g.line_h) - 1);

    // ---- mouse ----
    ImGui::SetCursorScreenPos(g.view_min);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton("##text", ImVec2(std::max(g.view_w(), 1.0f), std::max(g.view_h(), 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGui::PopItemFlag();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;
    const bool in_gutter = mouse.x < g.view_min.x + g.gutter_w;
    if (hovered && !in_gutter) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    auto line_range = [&](int line, TextPos& b, TextPos& e) {
        b = {line, 0};
        e = line + 1 < buf.line_count() ? TextPos{line + 1, 0}
                                        : TextPos{line, static_cast<int>(buf.line(line).size())};
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        const TextPos pos = hit_test(buf, g, mouse);
        if (in_gutter || clicks >= 3) {
            drag_ = DragMode::Line;
            line_range(pos.line, drag_begin_, drag_end_);
            if (io.KeyShift && in_gutter) {
                const TextPos a = buf.anchor();
                buf.set_selection(a, a <= drag_begin_ ? drag_end_ : drag_begin_);
            } else {
                buf.set_selection(drag_begin_, drag_end_);
            }
        } else if (clicks == 2) {
            drag_ = DragMode::Word;
            buf.select_word_at(pos);
            drag_begin_ = buf.selection_begin();
            drag_end_ = buf.selection_end();
        } else {
            drag_ = DragMode::Char;
            buf.set_cursor(pos, io.KeyShift);
        }
        blink_start_ = ImGui::GetTime();
    }
    if (drag_ != DragMode::None && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive()) {
        // Auto-scroll while dragging outside the view, faster the further out.
        const float dt = std::max(io.DeltaTime, 0.001f);
        float dy = 0.0f, dx = 0.0f;
        if (mouse.y < g.view_min.y) dy = mouse.y - g.view_min.y;
        if (mouse.y > g.view_max.y) dy = mouse.y - g.view_max.y;
        if (mouse.x < g.view_min.x + g.gutter_w && drag_ == DragMode::Char) dx = mouse.x - (g.view_min.x + g.gutter_w);
        if (mouse.x > g.view_max.x) dx = mouse.x - g.view_max.x;
        if (dy != 0.0f) {
            g.scroll_y = std::clamp(g.scroll_y + dy * dt * 12.0f, 0.0f, win->ScrollMax.y);
            ImGui::SetScrollY(g.scroll_y);
        }
        if (dx != 0.0f) {
            g.scroll_x = std::clamp(g.scroll_x + dx * dt * 12.0f, 0.0f, win->ScrollMax.x);
            ImGui::SetScrollX(g.scroll_x);
        }
        const TextPos pos = hit_test(buf, g, mouse);
        if (drag_ == DragMode::Char) {
            if (pos != buf.cursor()) buf.set_cursor(pos, true);
        } else {
            TextPos b, e;
            if (drag_ == DragMode::Line) {
                line_range(pos.line, b, e);
            } else {
                const TextPos a = buf.anchor(), c = buf.cursor();
                buf.select_word_at(pos);
                b = buf.selection_begin();
                e = buf.selection_end();
                buf.set_selection(a, c);
            }
            if (b < drag_begin_)
                buf.set_selection(drag_end_, b);
            else
                buf.set_selection(drag_begin_, std::max(e, drag_end_));
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = DragMode::None;

    // Context menu (right click moves the cursor unless it's inside the selection).
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const TextPos pos = hit_test(buf, g, mouse);
        if (!buf.has_selection() || pos < buf.selection_begin() || pos > buf.selection_end()) buf.set_cursor(pos);
        ImGui::SetWindowFocus();
        ImGui::OpenPopup("##code_context");
    }
    if (ImGui::BeginPopup("##code_context")) {
        const char* mod = io.ConfigMacOSXBehaviors ? "Cmd+" : "Ctrl+";
        auto label = [&](const char* key) { return std::string(mod) + key; };
        if (ImGui::MenuItem("Undo", label("Z").c_str(), false, !read_only && buf.can_undo())) buf.undo();
        if (ImGui::MenuItem("Redo", label("Shift+Z").c_str(), false, !read_only && buf.can_redo())) buf.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", label("X").c_str(), false, !read_only && buf.has_selection())) {
            ImGui::SetClipboardText(buf.selected_text().c_str());
            buf.backspace();
        }
        if (ImGui::MenuItem("Copy", label("C").c_str(), false, buf.has_selection()))
            ImGui::SetClipboardText(buf.selected_text().c_str());
        const char* clip = ImGui::GetClipboardText();
        if (ImGui::MenuItem("Paste", label("V").c_str(), false, !read_only && clip && *clip)) {
            buf.break_undo_group();
            buf.insert(clip);
            buf.break_undo_group();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", label("A").c_str())) buf.select_all();
        if (ImGui::MenuItem("Toggle Line Comment", label("/").c_str(), false, !read_only)) buf.toggle_line_comment();
        if (ImGui::MenuItem("Delete Line", label("Shift+K").c_str(), false, !read_only)) buf.delete_line();
        ImGui::EndPopup();
    }

    // ---- keyboard ----
    int scroll_lines = 0;
    if (focused_) {
        handle_keyboard(buf, read_only, page_lines, &scroll_lines);
        if (!read_only) handle_text_input(buf);
        if (!read_only) {
            // Tell the platform where the caret is (IME candidate window).
            ImGuiContext& gctx = *ImGui::GetCurrentContext();
            const TextPos c = buf.cursor();
            gctx.PlatformImeData.WantVisible = true;
            gctx.PlatformImeData.WantTextInput = true;
            gctx.PlatformImeData.InputPos = ImVec2(g.x_of(buf.visual_column(c.line, c.col)), g.y_of(c.line));
            gctx.PlatformImeData.InputLineHeight = g.line_h;
            gctx.PlatformImeData.ViewportId = win->Viewport->ID;
        }
    }

    // ---- reveal / keep the cursor visible ----
    if (reveal_line_ > 0) {
        const int line = std::clamp(reveal_line_ - 1, 0, buf.line_count() - 1);
        buf.set_cursor({line, 0});
        if (!buf.line(line).empty() && is_blank(buf.line(line)[0])) buf.move_home(false);
        reveal_line_ = 0;
        ensure_visible_ = true;
        center_next_ = true;
    }
    const TextPos cur = buf.cursor();
    if (cur != last_cursor_ || buf.version() != last_version_) {
        blink_start_ = ImGui::GetTime();
        if (cur != last_cursor_ && drag_ == DragMode::None) ensure_visible_ = true;
    }
    last_cursor_ = cur;
    last_version_ = buf.version();

    const float max_scroll_y = std::max(0.0f, content_height(g.view_h()) - g.view_h());
    if (scroll_lines != 0) {
        g.scroll_y = std::clamp(g.scroll_y + static_cast<float>(scroll_lines) * g.line_h, 0.0f, max_scroll_y);
        ImGui::SetScrollY(g.scroll_y);
    }
    if (ensure_visible_) {
        const float cy = static_cast<float>(cur.line) * g.line_h;
        float sy = g.scroll_y;
        if (center_next_) {
            // Revealed lines go to about a third of the way down, like jump-to-definition.
            if (cy < sy || cy + g.line_h > sy + g.view_h()) sy = cy - std::floor(g.view_h() * 0.33f);
        } else if (cy < sy) {
            sy = cy;
        } else if (cy + g.line_h > sy + g.view_h()) {
            sy = cy + g.line_h - g.view_h();
        }
        sy = std::clamp(sy, 0.0f, max_scroll_y);
        if (sy != g.scroll_y) {
            g.scroll_y = sy;
            ImGui::SetScrollY(sy);
        }
        const float cx = static_cast<float>(buf.visual_column(cur.line, cur.col)) * cw;
        const float tw = g.text_view_w() - text_pad;
        const float margin = std::min(cw * 6.0f, tw * 0.25f);
        float sx = g.scroll_x;
        if (cx < sx + margin)
            sx = std::max(0.0f, cx - margin);
        else if (cx > sx + tw - margin)
            sx = cx - tw + margin;
        if (sx != g.scroll_x) {
            g.scroll_x = sx;
            ImGui::SetScrollX(sx);
        }
        ensure_visible_ = false;
        center_next_ = false;
    }

    // ---- render ----
    ImDrawList* dl = win->DrawList;
    const int n = buf.line_count();
    const int first = std::clamp(static_cast<int>(g.scroll_y / g.line_h), 0, n - 1);
    const int last = std::clamp(static_cast<int>((g.scroll_y + g.view_h()) / g.line_h), 0, n - 1);
    first_visible_ = first;
    visible_lines_ = std::max(1, static_cast<int>(g.view_h() / g.line_h));
    const float text_left = g.view_min.x + g.gutter_w;
    const float text_dy = std::floor((g.line_h - fs) * 0.5f);
    const TextPos sel_b = buf.selection_begin(), sel_e = buf.selection_end();
    const bool has_sel = buf.has_selection();
    const int tab = buf.tab_size();

    dl->PushClipRect(ImVec2(text_left, g.view_min.y), g.view_max, true);

    // Current line.
    if (style.highlight_current_line && !has_sel && cur.line >= first && cur.line <= last) {
        const float y = g.y_of(cur.line);
        dl->AddRectFilled(ImVec2(text_left, y), ImVec2(g.view_max.x, y + g.line_h), theme::color(Color::CodeCurrentLine));
    }

    // Indent guides at the indentation of each enclosing block (the chain of
    // less-indented lines above), so aligned continuation lines don't get a
    // guide for every tab stop.
    if (style.indent_guides) {
        const ImU32 guide = IM_COL32(0x40, 0x40, 0x40, 0xFF);
        std::vector<int> stack;  // ancestor indents, increasing
        {
            int threshold = indent_of(buf, first);
            if (threshold < 0) threshold = 1 << 30;
            for (int k = first - 1; k >= std::max(0, first - 2000) && threshold > 0; --k) {
                const int ind = indent_of(buf, k);
                if (ind >= 0 && ind < threshold) {
                    stack.insert(stack.begin(), ind);
                    threshold = ind;
                }
            }
        }
        int prev_indent = stack.empty() ? 0 : stack.back();
        auto draw_guides = [&](int line, int below) {
            const float y = g.y_of(line);
            for (int c : stack) {
                if (c >= below) break;
                // A filled 1 px column stays crisp at any (fractional) cell width.
                const float x = std::floor(g.x_of(c));
                if (x >= text_left) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1.0f, y + g.line_h), guide);
            }
        };
        for (int l = first; l <= last; ++l) {
            const int ind = indent_of(buf, l);
            if (ind < 0) {
                // Blank line: continue the guides of the surrounding block.
                int next = 0;
                for (int k = l + 1; k < std::min(n, l + 200); ++k)
                    if ((next = indent_of(buf, k)) >= 0) break;
                draw_guides(l, std::max(prev_indent, std::max(next, 0)));
                continue;
            }
            while (!stack.empty() && stack.back() >= ind) stack.pop_back();
            draw_guides(l, ind);
            stack.push_back(ind);
            prev_indent = ind;
        }
    }

    // Find matches.
    if (!search_.empty()) {
        const ImU32 match = IM_COL32(234, 92, 0, 80);
        for (int l = first; l <= last; ++l) {
            const std::string& s = buf.line(l);
            for (size_t at = find_in(s, search_, 0, search_case_); at != std::string_view::npos;
                 at = find_in(s, search_, at + search_.size(), search_case_)) {
                const float x0 = g.x_of(buf.visual_column(l, static_cast<int>(at)));
                const float x1 = g.x_of(buf.visual_column(l, static_cast<int>(at + search_.size())));
                const float y = g.y_of(l);
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + g.line_h), match, 2.0f);
            }
        }
    }

    // Selection.
    if (has_sel) {
        const ImU32 col = focused_ ? theme::color(Color::CodeSelection)
                                   : scale_alpha(theme::color(Color::CodeSelection), 0.6f);
        for (int l = std::max(first, sel_b.line); l <= std::min(last, sel_e.line); ++l) {
            const int c0 = l == sel_b.line ? sel_b.col : 0;
            const int c1 = l == sel_e.line ? sel_e.col : static_cast<int>(buf.line(l).size());
            float x0 = g.x_of(buf.visual_column(l, c0));
            float x1 = g.x_of(buf.visual_column(l, c1));
            if (l < sel_e.line) x1 += std::floor(cw * 0.6f);  // the selected newline
            if (x1 <= x0) continue;
            const float y = g.y_of(l);
            // Multi-line selections are one continuous block; single-line ones get soft corners.
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + g.line_h), col, sel_b.line == sel_e.line ? 2.0f : 0.0f);
        }
    }

    // Matching bracket pair.
    TextPos br, mt;
    if (style.match_brackets && focused_ && !has_sel && buf.matching_bracket(cur, &br, &mt)) {
        for (const TextPos& p : {br, mt}) {
            if (p.line < first || p.line > last) continue;
            const float x = g.x_of(buf.visual_column(p.line, p.col));
            const float y = g.y_of(p.line);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + g.line_h), IM_COL32(0, 100, 0, 60));
            dl->AddRect(ImVec2(x, y), ImVec2(x + cw, y + g.line_h), IM_COL32(0x88, 0x88, 0x88, 0xFF));
        }
    }

    // Text, span by span; tabs advance to the next tab stop.
    const ImU32 text_col = token_color(TokenKind::Text);
    for (int l = first; l <= last; ++l) {
        const std::string& s = buf.line(l);
        const int len = static_cast<int>(s.size());
        const float ty = g.y_of(l) + text_dy;
        int v = 0;    // display column at `idx`
        int idx = 0;  // next byte to draw
        bool past_right = false;
        auto emit = [&](int end, ImU32 col) {
            end = std::min(end, len);
            int chunk = idx, chunk_v = v;
            auto flush = [&](int upto) {
                if (upto <= chunk || past_right) return;
                const float x = g.x_of(chunk_v);
                if (x > g.view_max.x) {
                    past_right = true;
                    return;
                }
                bool blank = true;
                for (int k = chunk; k < upto && blank; ++k) blank = s[k] == ' ';
                if (!blank) dl->AddText(font, fs, ImVec2(x, ty), col, s.data() + chunk, s.data() + upto);
            };
            for (int i = idx; i < end; ++i) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == '\t') {
                    flush(i);
                    v += tab - v % tab;
                    chunk = i + 1;
                    chunk_v = v;
                } else if (!is_cont(c)) {
                    ++v;
                }
            }
            flush(end);
            idx = std::max(idx, end);
        };
        for (const TokenSpan& span : hl.spans(l)) {
            if (past_right) break;
            if (span.start < idx || span.start >= len) continue;
            emit(span.start, text_col);
            emit(span.start + span.len, token_color(span.kind));
        }
        if (!past_right) emit(len, text_col);
    }

    // Caret (2 px, blinking; solid while typing/moving).
    if (focused_ && cur.line >= first && cur.line <= last) {
        const double t = ImGui::GetTime() - blink_start_;
        const bool on = t < 0.5 || std::fmod(t - 0.5, 1.0) >= 0.5;
        if (on) {
            // One pixel left of the cell so it sits between glyphs.
            const float x = std::floor(g.x_of(buf.visual_column(cur.line, cur.col))) - 1.0f;
            const float y = g.y_of(cur.line);
            dl->AddRectFilled(ImVec2(x, y + 1.0f), ImVec2(x + 2.0f, y + g.line_h - 1.0f), theme::color(Color::CodeCursor));
        }
    }
    dl->PopClipRect();

    // Gutter: drawn last so horizontally scrolled text slides under it.
    dl->PushClipRect(g.view_min, ImVec2(text_left, g.view_max.y), true);
    dl->AddRectFilled(g.view_min, ImVec2(text_left, g.view_max.y), theme::color(Color::CodeGutter));
    if (style.line_numbers) {
        char num[16];
        for (int l = first; l <= last; ++l) {
            const int len = std::snprintf(num, sizeof(num), "%d", l + 1);
            const float x = text_left - gutter_pad_r - static_cast<float>(len) * cw;
            const bool active = l == cur.line;
            dl->AddText(font, fs, ImVec2(x, g.y_of(l) + text_dy),
                        theme::color(active ? Color::CodeLineNumberActive : Color::CodeLineNumber), num, num + len);
        }
    }
    dl->PopClipRect();
    if (g.scroll_x > 0.0f) {
        // Shadow at the gutter edge while scrolled horizontally.
        const ImU32 dark = IM_COL32(0, 0, 0, 90), clear = IM_COL32(0, 0, 0, 0);
        dl->AddRectFilledMultiColor(ImVec2(text_left, g.view_min.y), ImVec2(text_left + 6.0f, g.view_max.y), dark, clear,
                                    clear, dark);
    }
    ImGui::EndChild();
    return buf.version() != version_before;
}

} // namespace osef::editor
