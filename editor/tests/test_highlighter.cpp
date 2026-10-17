// CppHighlighter / lex_cpp_line: token kinds, carried state, incremental re-lex.

#include "test_main.h"

#include "text/cpp_highlighter.h"
#include "text/text_buffer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <string_view>
#include <cstdint>

using namespace osef::editor;

namespace {

const char* kind_name(TokenKind k) {
    switch (k) {
        case TokenKind::Text: return "Text";
        case TokenKind::Keyword: return "Keyword";
        case TokenKind::Control: return "Control";
        case TokenKind::Type: return "Type";
        case TokenKind::Function: return "Function";
        case TokenKind::String: return "String";
        case TokenKind::Number: return "Number";
        case TokenKind::Comment: return "Comment";
        case TokenKind::Preprocessor: return "Preprocessor";
        case TokenKind::Macro: return "Macro";
        case TokenKind::Punctuation: return "Punctuation";
    }
    return "?";
}

// "text:Kind" for every span, joined by " | ".
std::string describe(std::string_view line, const std::vector<TokenSpan>& spans) {
    std::string out;
    for (const TokenSpan& s : spans) {
        if (!out.empty()) out += " | ";
        out += std::string(line.substr(s.start, s.len)) + ":" + kind_name(s.kind);
    }
    return out;
}

std::string lex(std::string_view line, LexState* state = nullptr) {
    LexState local;
    LexState& st = state ? *state : local;
    std::vector<TokenSpan> spans;
    lex_cpp_line(line, st, spans);
    return describe(line, spans);
}

// Kind of the span covering byte `col` (Text when none does).
TokenKind kind_at(std::string_view line, int col, LexState state = {}) {
    std::vector<TokenSpan> spans;
    lex_cpp_line(line, state, spans);
    for (const TokenSpan& s : spans)
        if (col >= s.start && col < s.start + s.len) return s.kind;
    return TokenKind::Text;
}

bool same_spans(const std::vector<TokenSpan>& a, const std::vector<TokenSpan>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].len != b[i].len || a[i].kind != b[i].kind) return false;
    return true;
}

// Every line of `inc` matches a fresh highlighter over the same buffer.
bool matches_full_relex(const CppHighlighter& inc, const TextBuffer& buf) {
    CppHighlighter full;
    full.update(buf);
    if (inc.line_count() != full.line_count()) return false;
    for (int i = 0; i < full.line_count(); ++i)
        if (!same_spans(inc.spans(i), full.spans(i)) || !(inc.state_at(i) == full.state_at(i))) return false;
    return true;
}

} // namespace

TEST(highlighter_keywords_types_functions) {
    CHECK_EQ(lex("const int x = foo(42);"),
             "const:Keyword | int:Type | x:Text | =:Punctuation | foo:Function | (:Punctuation | "
             "42:Number | );:Punctuation");
    CHECK_EQ(lex("if (a) return b; else co_return;"),
             "if:Control | (:Punctuation | a:Text | ):Punctuation | return:Control | b:Text | "
             ";:Punctuation | else:Control | co_return:Control | ;:Punctuation");
    CHECK_EQ(lex("std::vector<uint8_t> v;"),
             "std:Text | :::Punctuation | vector:Type | <:Punctuation | uint8_t:Type | >:Punctuation | "
             "v:Text | ;:Punctuation");
    CHECK_EQ(lex("class Kernel : public Base {"),
             "class:Keyword | Kernel:Type | ::Punctuation | public:Keyword | Base:Type | {:Punctuation");
    CHECK_EQ(lex("namespace osef {"), "namespace:Keyword | osef:Type | {:Punctuation");
    CHECK_EQ(lex("auto p = nullptr; bool t = true;"),
             "auto:Keyword | p:Text | =:Punctuation | nullptr:Keyword | ;:Punctuation | bool:Type | "
             "t:Text | =:Punctuation | true:Keyword | ;:Punctuation");
    CHECK_EQ(lex("k.print (s);"), "k:Text | .:Punctuation | print:Function | (:Punctuation | s:Text | );:Punctuation");
    CHECK(kind_at("for (;;) {}", 0) == TokenKind::Control);
    CHECK(kind_at("    switch (x) { case 1: break; }", 4) == TokenKind::Control);
    CHECK(kind_at("template <typename T> T f();", 22) == TokenKind::Type);
}

TEST(highlighter_macros) {
    CHECK_EQ(lex("OSEF_HARDWARE(osef::Hardware()"),
             "OSEF_HARDWARE:Macro | (:Punctuation | osef:Text | :::Punctuation | Hardware:Type | ():Punctuation");
    CHECK(kind_at("int n = MAX_LEN;", 8) == TokenKind::Macro);
    CHECK(kind_at("OSEF_assert(x);", 0) == TokenKind::Macro);
    CHECK(kind_at("T value;", 0) == TokenKind::Type);  // single capital: template parameter
    CHECK(kind_at("int A1_B;", 4) == TokenKind::Macro);
}

TEST(highlighter_numbers) {
    CHECK_EQ(lex("0x1F 0b1010 1'000'000 3.14f 1e-9 .5 42ull 0x1p+3"),
             "0x1F:Number | 0b1010:Number | 1'000'000:Number | 3.14f:Number | 1e-9:Number | .5:Number | "
             "42ull:Number | 0x1p+3:Number");
    CHECK_EQ(lex("a-1"), "a:Text | -:Punctuation | 1:Number");
    CHECK_EQ(lex("0xE+1"), "0xE:Number | +:Punctuation | 1:Number");  // no exponent sign in hex 'E'
}

TEST(highlighter_strings_and_chars) {
    CHECK_EQ(lex(R"(s = "a \"q\" b"; c = '\'';)"),
             R"(s:Text | =:Punctuation | "a \"q\" b":String | ;:Punctuation | c:Text | =:Punctuation | '\'':String | ;:Punctuation)");
    CHECK_EQ(lex(R"(u8"x" L'y' U"z")"), R"(u8"x":String | L'y':String | U"z":String)");
    CHECK_EQ(lex(R"x(R"(raw "text")" + 1)x"), R"x(R"(raw "text")":String | +:Punctuation | 1:Number)x");
    CHECK_EQ(lex(R"x(R"d(a)" b)d" x)x"), R"x(R"d(a)" b)d":String | x:Text)x");
    CHECK_EQ(lex("\"unterminated"), "\"unterminated:String");
    CHECK_EQ(lex("u8 x"), "u8:Text | x:Text");  // a prefix only counts before a quote
}

TEST(highlighter_comments) {
    CHECK_EQ(lex("x = 1; // note: \"not a string\""),
             "x:Text | =:Punctuation | 1:Number | ;:Punctuation | // note: \"not a string\":Comment");
    CHECK_EQ(lex("a /* b */ c"), "a:Text | /* b */:Comment | c:Text");
    CHECK_EQ(lex("s = \"// not a comment\";"), "s:Text | =:Punctuation | \"// not a comment\":String | ;:Punctuation");
}

TEST(highlighter_block_comment_state_across_lines) {
    LexState st;
    CHECK_EQ(lex("int a; /* start", &st), "int:Type | a:Text | ;:Punctuation | /* start:Comment");
    CHECK(st.mode == LexState::Mode::BlockComment);
    CHECK_EQ(lex("  still if (x) comment", &st), "  still if (x) comment:Comment");
    CHECK(st.mode == LexState::Mode::BlockComment);
    CHECK_EQ(lex("end */ return 0;", &st), "end */:Comment | return:Control | 0:Number | ;:Punctuation");
    CHECK(st.mode == LexState::Mode::Normal);
}

TEST(highlighter_raw_string_across_lines) {
    LexState st;
    CHECK_EQ(lex("auto s = R\"js(first", &st), "auto:Keyword | s:Text | =:Punctuation | R\"js(first:String");
    CHECK(st.mode == LexState::Mode::RawString);
    CHECK_EQ(st.raw_delim, "js");
    CHECK_EQ(lex("  )\" not the end // x", &st), "  )\" not the end // x:String");
    CHECK_EQ(lex("done)js\"; int y;", &st), "done)js\":String | ;:Punctuation | int:Type | y:Text | ;:Punctuation");
    CHECK(st.mode == LexState::Mode::Normal);
    CHECK(st.raw_delim.empty());
}

TEST(highlighter_preprocessor) {
    CHECK_EQ(lex("#include <osef/hardware.h>"), "#include:Preprocessor | <osef/hardware.h>:String");
    CHECK_EQ(lex("  #  include \"local.h\" // why"), "#  include:Preprocessor | \"local.h\":String | // why:Comment");
    CHECK_EQ(lex("#pragma once"), "#pragma:Preprocessor | once:Preprocessor");
    CHECK_EQ(lex("#if defined(__APPLE__) && X > 2"),
             "#if:Preprocessor | defined:Preprocessor | (:Punctuation | __APPLE__:Macro | ):Punctuation | "
             "&&:Punctuation | X:Type | >:Punctuation | 2:Number");
    // '<' outside #include is an operator.
    CHECK_EQ(lex("#if A < 3"), "#if:Preprocessor | A:Type | <:Punctuation | 3:Number");
    CHECK_EQ(lex("a < b > c"), "a:Text | <:Punctuation | b:Text | >:Punctuation | c:Text");

    LexState st;
    CHECK_EQ(lex("#define SQUARE(x) \\", &st), "#define:Preprocessor | SQUARE:Macro | (:Punctuation | x:Text | ):Punctuation | \\:Preprocessor");
    CHECK(st.preprocessor);
    // The continuation line has no directive of its own but is still part of it.
    CHECK_EQ(lex("    ((x) * (x)) \\", &st), "((:Punctuation | x:Text | ):Punctuation | *:Punctuation | (:Punctuation | x:Text | )):Punctuation | \\:Preprocessor");
    CHECK(st.preprocessor);
    CHECK_EQ(lex("    + 1", &st), "+:Punctuation | 1:Number");
    CHECK(!st.preprocessor);
    // A '#' on a normal line after a continuation ends is a directive again.
    CHECK_EQ(lex("#endif", &st), "#endif:Preprocessor");
}

TEST(highlighter_multibyte_text_does_not_break_spans) {
    // "// héllo" and an identifier with a non-ASCII letter.
    CHECK_EQ(lex("x = \"h\xC3\xA9\"; // \xC3\xA9t\xC3\xA9"),
             "x:Text | =:Punctuation | \"h\xC3\xA9\":String | ;:Punctuation | // \xC3\xA9t\xC3\xA9:Comment");
}

TEST(highlighter_incremental_matches_full) {
    TextBuffer buf(
        "#include <cstdio>\n"
        "// comment\n"
        "int main() {\n"
        "    const char* s = \"hi\";\n"
        "    return 0;\n"
        "}\n");
    CppHighlighter hl;
    CHECK(hl.update(buf));
    CHECK_EQ(hl.line_count(), 7);
    CHECK_EQ(hl.last_relexed_lines(), 7);
    CHECK(!hl.update(buf));  // same version: no work
    CHECK(hl.spans(2).size() > 0);
    CHECK(hl.spans(99).empty());

    // A one-line edit re-lexes just that line.
    buf.set_cursor({4, 11});
    buf.insert("1");
    CHECK(hl.update(buf));
    CHECK_EQ(hl.last_relexed_lines(), 1);
    CHECK(matches_full_relex(hl, buf));

    // Opening a block comment changes every later line's state...
    buf.set_cursor({1, 0});
    buf.insert("/* ");
    hl.update(buf);
    CHECK(hl.last_relexed_lines() >= 5);
    CHECK(hl.state_at(2).mode == LexState::Mode::BlockComment);
    CHECK(matches_full_relex(hl, buf));
    // ...and closing it again re-lexes until the cached states agree.
    buf.set_cursor({2, 0});
    buf.insert("*/");
    hl.update(buf);
    CHECK(matches_full_relex(hl, buf));
    CHECK(hl.state_at(3).mode == LexState::Mode::Normal);

    // Inserting and deleting whole lines.
    buf.set_cursor({0, 0});
    buf.insert("#define A \\\n  1\n");
    hl.update(buf);
    CHECK(matches_full_relex(hl, buf));
    CHECK(hl.state_at(1).preprocessor);
    buf.set_selection({0, 0}, {3, 0});
    buf.backspace();
    hl.update(buf);
    CHECK(matches_full_relex(hl, buf));
    buf.undo();
    hl.update(buf);
    CHECK(matches_full_relex(hl, buf));
    buf.set_text("");
    hl.update(buf);
    CHECK_EQ(hl.line_count(), 1);
    CHECK(matches_full_relex(hl, buf));
}

TEST(highlighter_incremental_randomized_edits_on_engine_source) {
    // A real file plus a deterministic sequence of edits: incremental == full every step.
    const auto path = osef::editor::test::repo_root() / "engine" / "src" / "kernel.cpp";
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) text = "int main() {\n  /* x */ return R\"(a)\";\n}\n";
    TextBuffer buf(text);
    CppHighlighter hl;
    hl.update(buf);
    const char* snippets[] = {"/*", "*/", "\"", "R\"x(", ")x\"", "\\", "\n", "#define Q \\\n", "{", "x"};
    uint32_t seed = 12345;
    auto rnd = [&](int n) {
        seed = seed * 1103515245u + 12345u;
        return static_cast<int>((seed >> 8) % static_cast<uint32_t>(n));
    };
    for (int step = 0; step < 60; ++step) {
        const int line = rnd(buf.line_count());
        buf.set_cursor({line, rnd(static_cast<int>(buf.line(line).size()) + 1)});
        if (step % 4 == 3) {
            buf.undo();
        } else {
            buf.insert(snippets[rnd(10)]);
        }
        hl.update(buf);
        if (!matches_full_relex(hl, buf)) {
            CHECK(false);
            break;
        }
    }
}
