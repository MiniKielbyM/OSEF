#pragma once
// Headless C++ syntax highlighter: lexes each line of a TextBuffer into
// colored token spans. Results are cached per line together with the lexer
// state carried into the next line (block comment, raw string, preprocessor
// continuation); after an edit only lines from the first changed one are
// re-lexed, stopping as soon as the carried state matches the cache again.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

class TextBuffer;

enum class TokenKind : uint8_t {
    Text,          // plain identifiers / anything uncolored
    Keyword,       // const, class, auto, nullptr, ...
    Control,       // if, for, return, co_await, ...
    Type,          // int, size_t, std::string, PascalCase names, ...
    Function,      // identifier followed by '('
    String,        // "..." '...' R"(...)" and <header> in #include
    Number,
    Comment,
    Preprocessor,  // #include, #define, ...
    Macro,         // ALL_CAPS identifiers, OSEF_*
    Punctuation,   // operators and brackets
};

// Byte range [start, start + len) of a line. Bytes outside every span are
// whitespace (TokenKind::Text).
struct TokenSpan {
    int start = 0;
    int len = 0;
    TokenKind kind = TokenKind::Text;
};

// Lexer state at a line boundary.
struct LexState {
    enum class Mode : uint8_t { Normal, BlockComment, RawString };
    Mode mode = Mode::Normal;
    bool preprocessor = false;  // the previous line was a directive ending in '\'
    std::string raw_delim;      // for Mode::RawString: the d-char sequence
    bool operator==(const LexState&) const = default;
};

// Lexes one line starting in `state`, appending spans to `out` (sorted, non
// overlapping) and updating `state` to the state at the end of the line.
void lex_cpp_line(std::string_view line, LexState& state, std::vector<TokenSpan>& out);

class CppHighlighter {
public:
    // Brings the cache up to date with `buf`. Cheap when buf.version() is
    // unchanged. Returns true if any line was re-lexed.
    bool update(const TextBuffer& buf);

    int line_count() const { return static_cast<int>(lines_.size()); }
    // Spans of `line` as of the last update(); empty when out of range.
    const std::vector<TokenSpan>& spans(int line) const;
    // State entering `line` (default state when out of range).
    LexState state_at(int line) const;

    void clear();                                           // forces a full re-lex on the next update
    // Plain text mode (non-C++ files): lines get no spans at all.
    void set_plain_text(bool plain);
    bool plain_text() const { return plain_; }
    int last_relexed_lines() const { return last_relexed_; } // lines lexed by the last update()

private:
    struct Line {
        std::string text;
        LexState in, out;
        std::vector<TokenSpan> spans;
    };
    std::vector<Line> lines_;
    uint64_t version_ = 0;
    bool valid_ = false;
    bool plain_ = false;
    int last_relexed_ = 0;
};

} // namespace osef::editor
