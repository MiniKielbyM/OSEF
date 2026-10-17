// TextBuffer: editing, selection, undo/redo, movement, find, saved state.

#include "test_main.h"

#include "text/text_buffer.h"

#include <ostream>
#include <string>

namespace osef::editor {
std::ostream& operator<<(std::ostream& os, const TextPos& p) { return os << "(" << p.line << ", " << p.col << ")"; }
} // namespace osef::editor

using osef::editor::TextBuffer;
using osef::editor::TextPos;

namespace {
void type(TextBuffer& b, std::string_view s) {
    for (char c : s) {
        if (c == '\n')
            b.newline();
        else
            b.insert(std::string(1, c));
    }
}
} // namespace

TEST(text_buffer_empty_and_load) {
    TextBuffer b;
    CHECK_EQ(b.line_count(), 1);
    CHECK_EQ(b.text(), "");
    CHECK(!b.modified());
    CHECK(!b.can_undo());

    b.set_text("a\nbc\n");
    CHECK_EQ(b.line_count(), 3);
    CHECK_EQ(b.line(1), "bc");
    CHECK_EQ(b.line(2), "");
    CHECK_EQ(b.line(99), "");   // clamped to the last line
    CHECK_EQ(b.line(-5), "a");  // clamped to the first
    CHECK_EQ(b.text(), "a\nbc\n");
    CHECK(!b.modified());
}

TEST(text_buffer_crlf_load) {
    TextBuffer b("one\r\ntwo\r\nthree");
    CHECK_EQ(b.line_count(), 3);
    CHECK_EQ(b.line(0), "one");
    CHECK_EQ(b.line(1), "two");
    CHECK_EQ(b.text(), "one\ntwo\nthree");
    b.set_text("x\ry");  // lone CR is a line break too
    CHECK_EQ(b.line_count(), 2);
    // Pasted CRLF text is normalized as well.
    b.set_text("");
    b.insert("p\r\nq");
    CHECK_EQ(b.text(), "p\nq");
}

TEST(text_buffer_version_increments) {
    TextBuffer b("abc");
    const uint64_t v0 = b.version();
    b.set_cursor({0, 3});
    CHECK_EQ(b.version(), v0);  // cursor moves are not content changes
    b.insert("d");
    const uint64_t v1 = b.version();
    CHECK(v1 > v0);
    b.undo();
    CHECK(b.version() > v1);
    const uint64_t v2 = b.version();
    b.redo();
    CHECK(b.version() > v2);
}

TEST(text_buffer_insert_and_cursor) {
    TextBuffer b;
    b.insert("hello");
    CHECK_EQ(b.text(), "hello");
    CHECK_EQ(b.cursor(), (TextPos{0, 5}));
    b.set_cursor({0, 0});
    b.insert("> ");
    CHECK_EQ(b.text(), "> hello");
    CHECK_EQ(b.cursor(), (TextPos{0, 2}));
}

TEST(text_buffer_multiline_insert) {
    TextBuffer b("start end");
    b.set_cursor({0, 6});
    b.insert("one\ntwo\nthree ");
    CHECK_EQ(b.line_count(), 3);
    CHECK_EQ(b.line(0), "start one");
    CHECK_EQ(b.line(1), "two");
    CHECK_EQ(b.line(2), "three end");
    CHECK_EQ(b.cursor(), (TextPos{2, 6}));
    CHECK(b.undo());
    CHECK_EQ(b.text(), "start end");
    CHECK_EQ(b.cursor(), (TextPos{0, 6}));
}

TEST(text_buffer_delete_across_lines) {
    TextBuffer b("alpha\nbeta\ngamma\ndelta");
    b.set_selection({0, 2}, {2, 3});
    CHECK_EQ(b.selected_text(), "pha\nbeta\ngam");
    b.backspace();
    CHECK_EQ(b.text(), "alma\ndelta");
    CHECK_EQ(b.cursor(), (TextPos{0, 2}));
    CHECK(!b.has_selection());
    b.undo();
    CHECK_EQ(b.text(), "alpha\nbeta\ngamma\ndelta");
    CHECK_EQ(b.selection_begin(), (TextPos{0, 2}));
    CHECK_EQ(b.selection_end(), (TextPos{2, 3}));

    // Backspace at column 0 joins with the previous line; delete at the end joins the next.
    b.set_cursor({1, 0});
    b.backspace();
    CHECK_EQ(b.line(0), "alphabeta");
    CHECK_EQ(b.cursor(), (TextPos{0, 5}));
    b.set_cursor({0, 9});
    b.delete_forward();
    CHECK_EQ(b.line(0), "alphabetagamma");
    CHECK_EQ(b.line_count(), 2);
    // Nothing to delete at the very ends.
    b.set_cursor({0, 0});
    b.backspace();
    b.move_doc_end(false);
    b.delete_forward();
    CHECK_EQ(b.text(), "alphabetagamma\ndelta");
}

TEST(text_buffer_typing_replaces_selection) {
    TextBuffer b("hello world");
    b.set_selection({0, 6}, {0, 11});
    b.insert("x");
    CHECK_EQ(b.text(), "hello x");
    b.insert("yz");
    CHECK_EQ(b.text(), "hello xyz");
    b.set_selection({0, 0}, {0, 5});
    b.insert("");  // empty insert deletes the selection
    CHECK_EQ(b.text(), " xyz");
}

TEST(text_buffer_selection_ordering) {
    TextBuffer b("abc\ndef");
    b.set_selection({1, 2}, {0, 1});  // anchor after the cursor (selected leftwards)
    CHECK(b.has_selection());
    CHECK_EQ(b.anchor(), (TextPos{1, 2}));
    CHECK_EQ(b.cursor(), (TextPos{0, 1}));
    CHECK_EQ(b.selection_begin(), (TextPos{0, 1}));
    CHECK_EQ(b.selection_end(), (TextPos{1, 2}));
    CHECK_EQ(b.selected_text(), "bc\nde");
    b.set_cursor({1, 3}, true);  // extend from the same anchor
    CHECK_EQ(b.anchor(), (TextPos{1, 2}));
    CHECK_EQ(b.selected_text(), "f");
    b.select_all();
    CHECK_EQ(b.selected_text(), "abc\ndef");
    b.clear_selection();
    CHECK(!b.has_selection());
    CHECK_EQ(b.clamp({7, 99}), (TextPos{1, 3}));
    CHECK_EQ(b.clamp({-1, 4}), (TextPos{0, 0}));
    CHECK_EQ(b.clamp({0, 99}), (TextPos{0, 3}));
}

TEST(text_buffer_select_word_and_line) {
    TextBuffer b("int foo_bar = 42; // x\nnext");
    b.select_word_at({0, 6});
    CHECK_EQ(b.selected_text(), "foo_bar");
    b.select_word_at({0, 11});  // after the word (on the space): previous word
    CHECK_EQ(b.selected_text(), "foo_bar");
    b.select_word_at({0, 12});  // on '=': single character
    CHECK_EQ(b.selected_text(), "=");
    b.select_word_at({0, 14});
    CHECK_EQ(b.selected_text(), "42");
    b.select_line(0);
    CHECK_EQ(b.selected_text(), "int foo_bar = 42; // x\n");
    b.select_line(1);  // last line: no newline to include
    CHECK_EQ(b.selected_text(), "next");
}

TEST(text_buffer_undo_redo_basic_and_redo_invalidation) {
    TextBuffer b("x");
    b.set_cursor({0, 1});
    b.insert("A");
    b.break_undo_group();
    b.insert("B");
    CHECK_EQ(b.text(), "xAB");
    CHECK(b.undo());
    CHECK_EQ(b.text(), "xA");
    CHECK(b.can_redo());
    CHECK(b.redo());
    CHECK_EQ(b.text(), "xAB");
    CHECK(b.undo());
    CHECK(b.undo());
    CHECK_EQ(b.text(), "x");
    CHECK(!b.undo());
    CHECK(b.can_redo());
    b.insert("Z");  // a new edit drops the redo history
    CHECK(!b.can_redo());
    CHECK(!b.redo());
    CHECK_EQ(b.text(), "xZ");
}

TEST(text_buffer_typing_coalesces) {
    TextBuffer b;
    type(b, "hello");
    CHECK_EQ(b.undo_depth(), 1);
    b.undo();
    CHECK_EQ(b.text(), "");

    // Whitespace -> word transitions start a new step.
    type(b, "int value");
    CHECK_EQ(b.undo_depth(), 2);
    b.undo();
    CHECK_EQ(b.text(), "int ");
    b.undo();
    CHECK_EQ(b.text(), "");

    // break_undo_group and cursor moves stop merging.
    type(b, "ab");
    b.break_undo_group();
    type(b, "cd");
    CHECK_EQ(b.undo_depth(), 2);
    b.move_left(false, false);
    b.move_right(false, false);
    type(b, "e");
    CHECK_EQ(b.undo_depth(), 3);
    b.undo();
    CHECK_EQ(b.text(), "abcd");

    // A newline is its own step; typing on the next line is separate.
    b.move_doc_end(false);
    type(b, "\nxy");
    CHECK_EQ(b.text(), "abcd\nxy");
    b.undo();
    CHECK_EQ(b.text(), "abcd\n");
    b.undo();
    CHECK_EQ(b.text(), "abcd");
    // Multi-character inserts (paste) are never merged into typing.
    b.insert("P");
    b.insert("paste");
    CHECK_EQ(b.text(), "abcdPpaste");
    b.undo();
    CHECK_EQ(b.text(), "abcdP");
}

TEST(text_buffer_backspace_coalesces) {
    TextBuffer b("abcdef");
    b.move_doc_end(false);
    b.backspace();
    b.backspace();
    b.backspace();
    CHECK_EQ(b.text(), "abc");
    const size_t depth = b.undo_depth();
    CHECK_EQ(depth, 1);
    b.undo();
    CHECK_EQ(b.text(), "abcdef");
    CHECK_EQ(b.cursor(), (TextPos{0, 6}));
    b.set_cursor({0, 0});
    b.delete_forward();
    b.delete_forward();
    CHECK_EQ(b.text(), "cdef");
    CHECK_EQ(b.undo_depth(), 1);
    b.undo();
    CHECK_EQ(b.text(), "abcdef");
}

TEST(text_buffer_utf8_movement_and_deletion) {
    // "aé€😀b": 1 + 2 + 3 + 4 + 1 bytes
    TextBuffer b("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "b");
    b.set_cursor({0, 0});
    b.move_right(false, false);
    CHECK_EQ(b.cursor().col, 1);
    b.move_right(false, false);
    CHECK_EQ(b.cursor().col, 3);
    b.move_right(false, false);
    CHECK_EQ(b.cursor().col, 6);
    b.move_right(false, false);
    CHECK_EQ(b.cursor().col, 10);
    b.move_left(false, false);
    CHECK_EQ(b.cursor().col, 6);
    // Clamping lands on a boundary, never inside a code point.
    CHECK_EQ(b.clamp({0, 8}).col, 6);
    b.set_cursor({0, 4});
    CHECK_EQ(b.cursor().col, 3);
    // Deleting removes whole code points.
    b.set_cursor({0, 10});
    b.backspace();
    CHECK_EQ(b.line(0), "a\xC3\xA9\xE2\x82\xAC" "b");
    b.set_cursor({0, 1});
    b.delete_forward();
    CHECK_EQ(b.line(0), "a\xE2\x82\xAC" "b");
    // Typing a multibyte character is one code point.
    b.insert("\xC3\xBC");
    CHECK_EQ(b.cursor().col, 3);
    CHECK_EQ(b.visual_column(0, 3), 2);
}

TEST(text_buffer_word_moves) {
    TextBuffer b("  foo_bar(baz, 42);  // done");
    b.set_cursor({0, 0});
    b.move_right(false, true);
    CHECK_EQ(b.cursor().col, 9);   // end of foo_bar
    b.move_right(false, true);
    CHECK_EQ(b.cursor().col, 10);  // over "("
    b.move_right(false, true);
    CHECK_EQ(b.cursor().col, 13);  // end of baz
    b.move_right(false, true);
    CHECK_EQ(b.cursor().col, 14);  // ","
    b.move_right(false, true);
    CHECK_EQ(b.cursor().col, 17);  // " 42"
    b.move_left(false, true);
    CHECK_EQ(b.cursor().col, 15);  // start of 42
    b.move_left(false, true);
    CHECK_EQ(b.cursor().col, 13);  // start of ","
    b.move_left(true, true);
    CHECK_EQ(b.cursor().col, 10);  // start of baz, selecting
    CHECK_EQ(b.selected_text(), "baz");

    // Word moves wrap across lines.
    TextBuffer c("ab\ncd");
    c.set_cursor({0, 2});
    c.move_right(false, true);
    CHECK_EQ(c.cursor(), (TextPos{1, 0}));
    c.move_left(false, true);
    CHECK_EQ(c.cursor(), (TextPos{0, 2}));
}

TEST(text_buffer_left_right_collapse_selection) {
    TextBuffer b("abcdef");
    b.set_selection({0, 1}, {0, 4});
    b.move_left(false, false);
    CHECK_EQ(b.cursor().col, 1);
    CHECK(!b.has_selection());
    b.set_selection({0, 4}, {0, 1});
    b.move_right(false, false);
    CHECK_EQ(b.cursor().col, 4);
    b.move_right(true, false);
    CHECK_EQ(b.selected_text(), "e");
}

TEST(text_buffer_up_down_keep_column) {
    TextBuffer b("a long line\nab\nanother long line");
    b.set_cursor({0, 8});
    b.move_down(false);
    CHECK_EQ(b.cursor(), (TextPos{1, 2}));  // clamped on the short line
    b.move_down(false);
    CHECK_EQ(b.cursor(), (TextPos{2, 8}));  // column remembered
    b.move_down(false);
    CHECK_EQ(b.cursor(), (TextPos{2, 17}));  // past the last line: end
    b.move_up(true);
    CHECK_EQ(b.anchor(), (TextPos{2, 17}));
    b.move_doc_start(false);
    b.move_up(false);
    CHECK_EQ(b.cursor(), (TextPos{0, 0}));
    b.move_lines(10, false);
    CHECK_EQ(b.cursor().line, 2);
    b.move_lines(-1, false);
    CHECK_EQ(b.cursor().line, 1);

    // Tabs count as display columns for vertical movement.
    TextBuffer t("\tx\n12345");
    t.set_cursor({0, 1});  // after the tab, display column 4
    t.move_down(false);
    CHECK_EQ(t.cursor(), (TextPos{1, 4}));
}

TEST(text_buffer_smart_home_and_end) {
    TextBuffer b("    return 0;");
    b.set_cursor({0, 10});
    b.move_home(false);
    CHECK_EQ(b.cursor().col, 4);
    b.move_home(false);
    CHECK_EQ(b.cursor().col, 0);
    b.move_home(false);
    CHECK_EQ(b.cursor().col, 4);
    b.move_end(true);
    CHECK_EQ(b.selected_text(), "return 0;");
    b.move_doc_start(true);
    CHECK_EQ(b.cursor(), (TextPos{0, 0}));
}

TEST(text_buffer_newline_auto_indent) {
    TextBuffer b("    int x;");
    b.move_doc_end(false);
    b.newline();
    CHECK_EQ(b.line(1), "    ");
    CHECK_EQ(b.cursor(), (TextPos{1, 4}));

    TextBuffer c("void f() {");
    c.move_doc_end(false);
    c.newline();
    CHECK_EQ(c.line(1), "    ");
    CHECK_EQ(c.cursor(), (TextPos{1, 4}));

    // Between braces the closing one moves to its own line.
    TextBuffer d("    if (x) {}");
    d.set_cursor({0, 12});
    d.newline();
    CHECK_EQ(d.text(), "    if (x) {\n        \n    }");
    CHECK_EQ(d.cursor(), (TextPos{1, 8}));
    d.undo();
    CHECK_EQ(d.text(), "    if (x) {}");

    // Enter on a blank indented line doesn't leave trailing spaces behind.
    TextBuffer e("    ");
    e.move_doc_end(false);
    e.newline();
    CHECK_EQ(e.text(), "\n    ");

    // Typing '}' on a blank line removes one indent level.
    TextBuffer f("{\n        ");
    f.move_doc_end(false);
    f.insert("}");
    CHECK_EQ(f.line(1), "    }");
    TextBuffer h("{\n      ");  // off a tab stop: back to the previous one
    h.move_doc_end(false);
    h.insert("}");
    CHECK_EQ(h.line(1), "    }");
    h.undo();
    CHECK_EQ(h.line(1), "      ");
}

TEST(text_buffer_backspace_in_leading_spaces) {
    TextBuffer b("      x");
    b.set_cursor({0, 6});
    b.backspace();
    CHECK_EQ(b.line(0), "    x");  // back to the tab stop at 4
    b.backspace();
    CHECK_EQ(b.line(0), "x");
    TextBuffer c("ab  x");
    c.set_cursor({0, 4});
    c.backspace();
    CHECK_EQ(c.line(0), "ab x");  // not leading: a single character
}

TEST(text_buffer_indent_unindent) {
    TextBuffer b("ab");
    b.set_cursor({0, 1});
    b.indent();
    CHECK_EQ(b.line(0), "a   b");  // spaces to the next tab stop
    CHECK_EQ(b.cursor().col, 4);
    b.undo();

    TextBuffer m("one\n\ntwo\nthree");
    m.set_selection({0, 1}, {2, 2});
    m.indent();
    CHECK_EQ(m.text(), "    one\n\n    two\nthree");  // blank lines untouched
    CHECK_EQ(m.anchor(), (TextPos{0, 5}));
    CHECK_EQ(m.cursor(), (TextPos{2, 6}));
    CHECK_EQ(m.undo_depth(), 1);
    m.unindent();
    CHECK_EQ(m.text(), "one\n\ntwo\nthree");
    CHECK_EQ(m.anchor(), (TextPos{0, 1}));
    CHECK_EQ(m.cursor(), (TextPos{2, 2}));
    m.undo();
    m.undo();
    CHECK_EQ(m.text(), "one\n\ntwo\nthree");

    // A selection ending at column 0 doesn't include that line.
    m.set_selection({0, 0}, {2, 0});
    m.indent();
    CHECK_EQ(m.text(), "    one\n\ntwo\nthree");

    // Unindent removes at most one level, or one tab, and works without a selection.
    TextBuffer u("      a\n\tb\n  c");
    u.set_selection({0, 0}, {2, 3});
    u.unindent();
    CHECK_EQ(u.text(), "  a\nb\nc");
    u.set_cursor({0, 3});
    u.unindent();
    CHECK_EQ(u.text(), "a\nb\nc");
    CHECK_EQ(u.cursor(), (TextPos{0, 1}));
}

TEST(text_buffer_toggle_line_comment) {
    TextBuffer b("    int a;\n\n        int b;");
    b.set_selection({0, 0}, {2, 5});
    b.toggle_line_comment();
    CHECK_EQ(b.text(), "    // int a;\n\n    //     int b;");
    CHECK_EQ(b.undo_depth(), 1);
    b.toggle_line_comment();
    CHECK_EQ(b.text(), "    int a;\n\n        int b;");
    // Mixed: some lines commented -> comment all.
    TextBuffer c("// a\nb");
    c.select_all();
    c.toggle_line_comment();
    CHECK_EQ(c.text(), "// // a\n// b");
    // Without a selection: the cursor's line, the cursor moves with the text.
    TextBuffer d("x = 1;");
    d.set_cursor({0, 2});
    d.toggle_line_comment();
    CHECK_EQ(d.text(), "// x = 1;");
    CHECK_EQ(d.cursor().col, 5);
    d.toggle_line_comment();
    CHECK_EQ(d.text(), "x = 1;");
    CHECK_EQ(d.cursor().col, 2);
    // "//x" without the space uncomments too.
    TextBuffer e("//x");
    e.toggle_line_comment();
    CHECK_EQ(e.text(), "x");
}

TEST(text_buffer_delete_line) {
    TextBuffer b("one\ntwo\nthree");
    b.set_cursor({1, 2});
    b.delete_line();
    CHECK_EQ(b.text(), "one\nthree");
    CHECK_EQ(b.cursor(), (TextPos{1, 2}));
    b.move_doc_end(false);
    b.delete_line();  // last line: removes the preceding newline
    CHECK_EQ(b.text(), "one");
    b.delete_line();
    CHECK_EQ(b.text(), "");
    CHECK_EQ(b.line_count(), 1);
    b.undo();
    b.undo();
    b.undo();
    CHECK_EQ(b.text(), "one\ntwo\nthree");
}

TEST(text_buffer_replace_all_single_undo_step) {
    TextBuffer b("line 0\nline 1\nline 2\nline 3");
    b.set_cursor({2, 3});
    const size_t depth = b.undo_depth();
    b.replace_all("LINE 0\nline 1\nline two\nline 3\nline 4");
    CHECK_EQ(b.text(), "LINE 0\nline 1\nline two\nline 3\nline 4");
    CHECK_EQ(b.undo_depth(), depth + 1);
    CHECK_EQ(b.cursor().line, 2);  // cursor stays on its line
    CHECK(b.modified());
    b.undo();
    CHECK_EQ(b.text(), "line 0\nline 1\nline 2\nline 3");
    CHECK(!b.modified());
    b.redo();
    CHECK_EQ(b.text(), "LINE 0\nline 1\nline two\nline 3\nline 4");

    // Identical text is a no-op; the cursor is clamped if its line disappears.
    const uint64_t v = b.version();
    b.replace_all(b.text());
    CHECK_EQ(b.version(), v);
    b.set_cursor({4, 6});
    b.replace_all("short");
    CHECK_EQ(b.cursor(), (TextPos{0, 5}));
    // Multibyte characters at the edit boundary stay intact.
    TextBuffer u("x\xC3\xA9y");
    u.replace_all("x\xC3\xA8y");
    CHECK_EQ(u.text(), "x\xC3\xA8y");
    u.undo();
    CHECK_EQ(u.text(), "x\xC3\xA9y");
}

TEST(text_buffer_find_wrap_and_case) {
    TextBuffer b("Foo bar\nfoo baz\nFOO");
    b.set_cursor({0, 0});
    CHECK(b.find("foo", true, true));
    CHECK_EQ(b.selection_begin(), (TextPos{1, 0}));
    CHECK_EQ(b.selected_text(), "foo");
    CHECK(b.find("foo", true, true));  // wraps around to the only match: itself
    CHECK_EQ(b.selection_begin(), (TextPos{1, 0}));

    b.set_cursor({0, 0});
    CHECK(b.find("foo", true, false));
    CHECK_EQ(b.selection_begin(), (TextPos{0, 0}));
    CHECK(b.find("foo", true, false));
    CHECK_EQ(b.selection_begin(), (TextPos{1, 0}));
    CHECK(b.find("foo", true, false));
    CHECK_EQ(b.selection_begin(), (TextPos{2, 0}));
    CHECK(b.find("foo", true, false));  // wrap
    CHECK_EQ(b.selection_begin(), (TextPos{0, 0}));
    CHECK(b.find("foo", false, false));  // backward wrap
    CHECK_EQ(b.selection_begin(), (TextPos{2, 0}));
    CHECK(b.find("foo", false, false));
    CHECK_EQ(b.selection_begin(), (TextPos{1, 0}));

    // No wrap.
    b.set_cursor({2, 0});
    CHECK(b.find("FOO", true, true, false));
    CHECK(!b.find("FOO", true, true, false));
    CHECK(!b.find("zzz", true, false));
    CHECK(!b.find("", true, false));

    CHECK_EQ(b.find_all("foo", false).size(), 3);
    CHECK_EQ(b.find_all("foo", true).size(), 1);
    CHECK_EQ(b.find_all("o", true).size(), 4);
    TextBuffer r("aaaa");
    CHECK_EQ(r.find_all("aa", true).size(), 2);  // non-overlapping
}

TEST(text_buffer_modified_tracks_save_point) {
    TextBuffer b("abc");
    CHECK(!b.modified());
    b.move_doc_end(false);
    b.insert("d");
    CHECK(b.modified());
    b.undo();
    CHECK(!b.modified());
    b.redo();
    CHECK(b.modified());
    b.mark_saved();
    CHECK(!b.modified());
    // Typing after a save must not merge into the saved step.
    b.insert("e");
    CHECK(b.modified());
    b.undo();
    CHECK(!b.modified());
    CHECK_EQ(b.text(), "abcd");
    b.undo();
    CHECK(b.modified());  // before the save point
    b.redo();
    CHECK(!b.modified());
    // Save point lost to redo invalidation stays "modified".
    b.undo();
    b.insert("x");
    CHECK(b.modified());
    b.undo();
    CHECK(b.modified());
    b.set_text("fresh");
    CHECK(!b.modified());
    CHECK(!b.can_undo());
}

TEST(text_buffer_matching_bracket_and_columns) {
    TextBuffer b("f(a[1], {\n  x\n})");
    TextPos br, mt;
    CHECK(b.matching_bracket({0, 1}, &br, &mt));
    CHECK_EQ(br, (TextPos{0, 1}));
    CHECK_EQ(mt, (TextPos{2, 1}));
    CHECK(b.matching_bracket({2, 2}, &br, &mt));  // just after ')'
    CHECK_EQ(br, (TextPos{2, 1}));
    CHECK_EQ(mt, (TextPos{0, 1}));
    CHECK(b.matching_bracket({0, 8}, &br, &mt));
    CHECK_EQ(mt, (TextPos{2, 0}));
    CHECK(!b.matching_bracket({1, 1}, &br, &mt));
    TextBuffer u("((");
    CHECK(!u.matching_bracket({0, 0}, &br, &mt));

    TextBuffer t("\ta\tb");
    CHECK_EQ(t.visual_column(0, 1), 4);
    CHECK_EQ(t.visual_column(0, 3), 8);
    CHECK_EQ(t.column_at_visual(0, 1.0f), 0);
    CHECK_EQ(t.column_at_visual(0, 3.0f), 1);
    CHECK_EQ(t.column_at_visual(0, 4.4f), 1);
    CHECK_EQ(t.column_at_visual(0, 99.0f), 4);
    t.set_tab_size(2);
    CHECK_EQ(t.visual_column(0, 3), 4);
    CHECK_EQ(t.tab_size(), 2);
}
