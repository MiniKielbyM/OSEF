#pragma once
// Editing model behind the code editor: lines, cursor + selection, undo/redo,
// find. Headless (no ImGui), so it is unit tested directly.
//
// Positions are 0-based; `col` is a byte offset into the line's UTF-8 text
// and is always kept on a code point boundary.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

struct TextPos {
    int line = 0;
    int col = 0;
    auto operator<=>(const TextPos&) const = default;
};

class TextBuffer {
public:
    TextBuffer();
    explicit TextBuffer(std::string_view text);
    ~TextBuffer();
    TextBuffer(const TextBuffer&) = delete;
    TextBuffer& operator=(const TextBuffer&) = delete;

    // ---- content ----
    std::string text() const;                  // lines joined with "\n"
    void set_text(std::string_view text);      // load: resets undo history and cursor; "\r\n" -> "\n"
    int line_count() const;                    // always >= 1
    const std::string& line(int index) const;  // index clamped to [0, line_count)
    uint64_t version() const;                  // increments on every content change, incl. undo/redo

    // ---- cursor and selection ----
    TextPos cursor() const;
    TextPos anchor() const;                    // == cursor() when there is no selection
    bool has_selection() const;
    TextPos selection_begin() const;           // min(anchor, cursor)
    TextPos selection_end() const;             // max(anchor, cursor)
    std::string selected_text() const;
    void set_cursor(TextPos pos, bool extend_selection = false);  // clamped
    void set_selection(TextPos anchor, TextPos cursor);           // clamped
    void select_all();
    void clear_selection();
    void select_word_at(TextPos pos);          // identifier/number run, else the single character
    void select_line(int line);                // whole line including its newline
    TextPos clamp(TextPos pos) const;

    // ---- editing (undoable; each first replaces the selection) ----
    void insert(std::string_view text);        // may contain newlines; consecutive typing merges into one undo step
    void newline();                            // inserts "\n" + the current line's indentation (+1 level after '{')
    void backspace();                          // deletes selection or previous code point (or one indent level in leading spaces)
    void delete_forward();
    void indent();                             // Tab: indents selected lines, or inserts spaces to the next tab stop
    void unindent();                           // Shift+Tab
    void toggle_line_comment();                // "// " on all selected lines
    void delete_line();                        // removes the cursor's line(s)
    void replace_all(std::string_view text);   // whole content as ONE undo step; cursor stays on the same line

    // ---- movement (extend = Shift held) ----
    void move_left(bool extend, bool word);
    void move_right(bool extend, bool word);
    void move_up(bool extend);
    void move_down(bool extend);
    void move_home(bool extend);               // smart home: first non-blank, then column 0
    void move_end(bool extend);
    void move_lines(int delta, bool extend);   // page up/down
    void move_doc_start(bool extend);
    void move_doc_end(bool extend);

    // ---- undo/redo ----
    bool can_undo() const;
    bool can_redo() const;
    bool undo();
    bool redo();
    void break_undo_group();                   // stops the next insert from merging with the previous one

    // ---- find ----
    // Selects the next/previous match starting at the cursor. Returns false if none.
    bool find(std::string_view needle, bool forward, bool case_sensitive, bool wrap = true);
    std::vector<TextPos> find_all(std::string_view needle, bool case_sensitive) const; // match starts

    // ---- saved state ----
    void mark_saved();                         // current state == file on disk
    bool modified() const;                     // false again after undoing back to the saved state

    int tab_size() const;
    void set_tab_size(int spaces);             // default 4

    // ---- helpers for views (tabs expanded to tab_size) ----
    int visual_column(int line, int col) const;          // display column of byte `col`
    int column_at_visual(int line, float vcol) const;    // nearest code point boundary to a display column
    // If `pos` is next to a bracket ()[]{} (the character at pos, else the one
    // before it), returns true with that bracket's position and its partner's.
    bool matching_bracket(TextPos pos, TextPos* bracket, TextPos* match) const;
    // Number of undo steps (used to tell an edit from a coalesced keystroke).
    size_t undo_depth() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace osef::editor
