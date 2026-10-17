#pragma once
// The code editor widget: draws a TextBuffer with syntax colors from a
// CppHighlighter and handles mouse + keyboard editing (Dear ImGui).
//
//   CodeEditor editor;                          // one per open document (keeps view state)
//   editor.draw("##code", buffer, highlighter, CodeEditorStyle{}, fonts.mono, read_only);
//
// Rendering is virtualized (only visible lines are drawn), so large files stay cheap.

#include "text/text_buffer.h"

#include "imgui.h"

#include <cstdint>
#include <string>

namespace osef::editor {

class CppHighlighter;

struct CodeEditorStyle {
    float font_size = 0.0f;        // 0 = the mono font's own size
    float line_spacing = 1.3f;     // line height = font size * line_spacing (ImGui font size ~ 1.3 em)
    bool line_numbers = true;
    bool highlight_current_line = true;
    bool indent_guides = true;
    bool match_brackets = true;
    float scroll_past_end = 0.6f;  // fraction of the view that can scroll past the last line
};

class CodeEditor {
public:
    // Draws the editor into a child window of `size` (0 = fill the available
    // region). Returns true if the buffer's content changed this frame.
    // `read_only` allows navigation, selection and copy only.
    bool draw(const char* id, TextBuffer& buf, CppHighlighter& hl, const CodeEditorStyle& style, ImFont* mono,
              bool read_only, ImVec2 size = ImVec2(0.0f, 0.0f));

    // Moves the cursor to `line` (1-based) and scrolls it to the middle of the view on the next draw.
    void reveal_line(int line);
    // Scrolls just enough to show the cursor on the next draw (e.g. after find).
    void scroll_to_cursor() { ensure_visible_ = true; }
    // Gives the editor keyboard focus on the next draw.
    void focus() { want_focus_ = true; }
    bool focused() const { return focused_; }

    // Occurrences of `needle` are highlighted behind the text (find bar). Empty = none.
    void set_search_highlight(std::string needle, bool case_sensitive);

    // Geometry of the last draw (for tests/tools): first visible line and visible line count.
    int first_visible_line() const { return first_visible_; }
    int visible_lines() const { return visible_lines_; }

private:
    enum class DragMode { None, Char, Word, Line };

    bool handle_keyboard(TextBuffer& buf, bool read_only, int page_lines, int* scroll_lines);
    void handle_text_input(TextBuffer& buf);
    int max_columns(const TextBuffer& buf);

    bool focused_ = false;
    bool want_focus_ = false;
    bool ensure_visible_ = false;
    int reveal_line_ = 0;  // pending, 1-based
    bool center_next_ = false;

    DragMode drag_ = DragMode::None;
    TextPos drag_begin_, drag_end_;  // the word/line under the initial click

    TextPos last_cursor_{-1, -1};
    uint64_t last_version_ = ~0ull;
    double blink_start_ = 0.0;

    uint64_t cols_version_ = ~0ull;
    int max_cols_ = 0;

    std::string search_;
    bool search_case_ = false;

    int first_visible_ = 0;
    int visible_lines_ = 0;
};

} // namespace osef::editor
