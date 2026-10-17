#pragma once
// Minimal C++ tokenizer for the model layer (hardware declaration parser and
// the Programs/Modules scanner). Skips whitespace and comments, and turns
// string/char literals (including raw strings and escapes) into single
// tokens, so code inside comments or strings is never mistaken for code.
// Not a full lexer: no preprocessing, no trigraphs.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor::cpp {

enum class TokKind {
    Identifier,  // also keywords
    Number,      // pp-number: 0x1Fu, 1'000, 1e5, ...
    String,      // "..." / R"(...)" with an optional encoding prefix; `value` is decoded
    Char,        // '...'
    Punct,       // one character, or "::" / "->"
};

struct Token {
    TokKind kind = TokKind::Punct;
    size_t begin = 0;      // byte offsets into the source
    size_t end = 0;
    std::string_view text; // source[begin, end)
    std::string value;     // String: decoded contents (UTF-8)
    bool ok = true;        // String/Char: terminated and all escapes valid

    bool is(std::string_view punct_or_ident) const {
        return (kind == TokKind::Punct || kind == TokKind::Identifier) && text == punct_or_ident;
    }
};

struct Range {
    size_t begin = 0;
    size_t end = 0;
};

struct Tokens {
    std::vector<Token> tokens;
    std::vector<Range> comments;  // comment byte ranges, in order
};

Tokens tokenize(std::string_view source);

// Decodes the body of an ordinary string literal (between the quotes).
// Returns false on an invalid escape (the output is still filled best-effort).
bool decode_escapes(std::string_view body, std::string& out);

// Index of the token closing the bracket opened at tokens[open] ("(" / "[" /
// "{"), or npos when unbalanced.
size_t match_bracket(const std::vector<Token>& tokens, size_t open);

// Tokens that the preprocessor keeps, for simple cases: directive lines
// (#include, #define bodies, #if ...) are dropped, and so is code in branches
// that are statically off (#if 0, the #else of #if 1, ...). Conditions the
// scanner cannot evaluate (#ifdef X, #if FOO > 2) keep all their branches.
struct ActiveCode {
    std::vector<Token> tokens;
    std::vector<Range> directives;  // byte ranges of directive lines (with continuations)
    bool uncertain = false;         // some kept code is under a condition that was not evaluated
};
ActiveCode active_code(std::string_view source, const Tokens& tokens);

// 1-based line of a byte offset.
int line_of(std::string_view source, size_t offset);

constexpr size_t npos = static_cast<size_t>(-1);

} // namespace osef::editor::cpp
