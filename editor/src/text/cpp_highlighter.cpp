// C++ lexer for syntax coloring (approximate by design: no parsing, just
// tokens plus a few contextual rules such as "identifier followed by '('").

#include "text/cpp_highlighter.h"

#include "text/text_buffer.h"

#include <algorithm>
#include <array>

namespace osef::editor {

namespace {

bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_ident(unsigned char c) { return is_alpha(c) || is_digit(c); }
bool is_blank(char c) { return c == ' ' || c == '\t'; }

template <size_t N>
constexpr std::array<std::string_view, N> sorted(std::array<std::string_view, N> words) {
    std::sort(words.begin(), words.end());
    return words;
}

template <size_t N>
bool contains(const std::array<std::string_view, N>& words, std::string_view w) {
    return std::binary_search(words.begin(), words.end(), w);
}

constexpr auto kControl = sorted(std::to_array<std::string_view>({
    "if", "else", "for", "while", "do", "return", "switch", "case", "default", "break", "continue",
    "goto", "co_await", "co_yield", "co_return", "try", "catch", "throw"}));

constexpr auto kKeywords = sorted(std::to_array<std::string_view>({
    "alignas", "alignof", "asm", "auto", "class", "concept", "const", "consteval", "constexpr",
    "constinit", "const_cast", "decltype", "delete", "dynamic_cast", "enum", "explicit", "export",
    "extern", "false", "final", "friend", "inline", "mutable", "namespace", "new", "noexcept",
    "nullptr", "operator", "override", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "sizeof", "static", "static_assert", "static_cast", "struct",
    "template", "this", "thread_local", "true", "typedef", "typeid", "typename", "union", "using",
    "virtual", "volatile", "and", "and_eq", "bitand", "bitor", "compl", "not", "not_eq", "or",
    "or_eq", "xor", "xor_eq", "import", "module", "__attribute__", "__declspec", "__asm__",
    "__volatile__", "__restrict"}));

constexpr auto kTypes = sorted(std::to_array<std::string_view>({
    // primitives
    "void", "bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long",
    "float", "double", "signed", "unsigned",
    // <cstdint> / <cstddef>
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "nullptr_t",
    "max_align_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "byte", "va_list",
    // std
    "string", "string_view", "wstring", "u8string", "vector", "array", "map", "multimap",
    "unordered_map", "set", "multiset", "unordered_set", "deque", "list", "forward_list", "queue",
    "priority_queue", "stack", "pair", "tuple", "optional", "variant", "any", "function", "span",
    "unique_ptr", "shared_ptr", "weak_ptr", "atomic", "mutex", "recursive_mutex", "lock_guard",
    "unique_lock", "scoped_lock", "condition_variable", "thread", "jthread", "path", "ostream",
    "istream", "initializer_list"}));

// Keywords after which the next identifier names a type.
constexpr auto kTypeIntroducers = sorted(std::to_array<std::string_view>({
    "class", "struct", "enum", "union", "namespace", "typename", "concept"}));

bool is_macro_name(std::string_view w) {
    if (w.starts_with("OSEF_")) return true;
    if (w.size() < 2) return false;
    bool upper = false;
    for (char c : w) {
        if (c >= 'A' && c <= 'Z')
            upper = true;
        else if (!(is_digit(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return upper;
}

bool is_pascal_case(std::string_view w) {
    if (w.empty() || !(w[0] >= 'A' && w[0] <= 'Z')) return false;
    if (w.size() == 1) return true;  // template parameters: T, U, K
    return std::any_of(w.begin(), w.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

bool is_string_prefix(std::string_view w) {
    static constexpr std::array<std::string_view, 9> kPrefixes = {"u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"};
    return std::find(kPrefixes.begin(), kPrefixes.end(), w) != kPrefixes.end();
}

class LineLexer {
public:
    LineLexer(std::string_view line, LexState& state, std::vector<TokenSpan>& out)
        : s_(line), n_(static_cast<int>(line.size())), st_(state), out_(out) {}

    void run() {
        int i = 0;
        bool pp = st_.preprocessor;
        if (st_.mode == LexState::Mode::BlockComment) {
            i = block_comment(0);
            if (st_.mode == LexState::Mode::BlockComment) return;
        } else if (st_.mode == LexState::Mode::RawString) {
            i = raw_string_body(0, 0);
            if (st_.mode == LexState::Mode::RawString) return;
        } else if (!pp) {
            int k = 0;
            while (k < n_ && is_blank(s_[k])) ++k;
            if (k < n_ && s_[k] == '#') {
                pp = true;
                i = directive(k);
            }
        }
        st_.preprocessor = false;

        while (i < n_) {
            const unsigned char c = static_cast<unsigned char>(s_[i]);
            if (is_blank(static_cast<char>(c))) {
                ++i;
            } else if (c == '/' && i + 1 < n_ && s_[i + 1] == '/') {
                push(i, n_, TokenKind::Comment);
                i = n_;
            } else if (c == '/' && i + 1 < n_ && s_[i + 1] == '*') {
                i = block_comment(i);
                if (st_.mode == LexState::Mode::BlockComment) break;
            } else if (include_ && c == '<') {
                size_t e = s_.find('>', static_cast<size_t>(i));
                const int end = e == std::string_view::npos ? n_ : static_cast<int>(e) + 1;
                push(i, end, TokenKind::String);
                i = end;
            } else if (c == '"' || c == '\'') {
                i = quoted(i, i, false);
                if (st_.mode == LexState::Mode::RawString) break;
            } else if (is_digit(c) || (c == '.' && i + 1 < n_ && is_digit(static_cast<unsigned char>(s_[i + 1])))) {
                i = number(i);
            } else if (is_alpha(c)) {
                i = identifier(i);
                if (st_.mode == LexState::Mode::RawString) break;
            } else {
                if (c == '\\' && i == n_ - 1 && pp) {
                    push(i, i + 1, TokenKind::Preprocessor);
                } else {
                    push(i, i + 1, TokenKind::Punctuation);
                }
                ++i;
            }
        }

        // A directive continues on the next line when this one ends with '\'.
        if (pp && st_.mode == LexState::Mode::Normal) {
            int e = n_;
            while (e > 0 && is_blank(s_[e - 1])) --e;
            st_.preprocessor = e > 0 && s_[e - 1] == '\\';
        } else if (pp) {
            st_.preprocessor = true;  // directive interrupted by a multi-line comment
        }
    }

private:
    void push(int start, int end, TokenKind kind) {
        if (end <= start) return;
        if (!out_.empty() && kind == TokenKind::Punctuation) {
            TokenSpan& last = out_.back();
            if (last.kind == kind && last.start + last.len == start) {
                last.len += end - start;
                return;
            }
        }
        out_.push_back({start, end - start, kind});
    }

    // "/* ... */" starting at `i` (or a continuation from the previous line when
    // the mode is already BlockComment). Returns the index after it.
    int block_comment(int i) {
        const int from = st_.mode == LexState::Mode::BlockComment ? i : i + 2;
        const size_t e = s_.find("*/", static_cast<size_t>(from));
        if (e == std::string_view::npos) {
            push(i, n_, TokenKind::Comment);
            st_.mode = LexState::Mode::BlockComment;
            return n_;
        }
        st_.mode = LexState::Mode::Normal;
        push(i, static_cast<int>(e) + 2, TokenKind::Comment);
        return static_cast<int>(e) + 2;
    }

    // Raw string content from `body` (after the opening '(') to the closing
    // )delim"; the span starts at `start`.
    int raw_string_body(int start, int body) {
        const std::string close = ")" + st_.raw_delim + "\"";
        const size_t e = s_.find(close, static_cast<size_t>(body));
        if (e == std::string_view::npos) {
            push(start, n_, TokenKind::String);
            st_.mode = LexState::Mode::RawString;
            return n_;
        }
        const int end = static_cast<int>(e + close.size());
        push(start, end, TokenKind::String);
        st_.mode = LexState::Mode::Normal;
        st_.raw_delim.clear();
        return end;
    }

    // A string or character literal whose quote is at `q`; the span starts at
    // `start` (the encoding prefix, if any).
    int quoted(int start, int q, bool raw) {
        const char quote = s_[q];
        if (raw && quote == '"') {
            const size_t paren = s_.find('(', static_cast<size_t>(q) + 1);
            if (paren != std::string_view::npos && paren - q - 1 <= 16) {
                const std::string_view delim = s_.substr(q + 1, paren - q - 1);
                if (delim.find_first_of(" \t\\)\"") == std::string_view::npos) {
                    st_.raw_delim = std::string(delim);
                    return raw_string_body(start, static_cast<int>(paren) + 1);
                }
            }
        }
        int j = q + 1;
        while (j < n_) {
            if (s_[j] == '\\') {
                j += 2;
            } else if (s_[j] == quote) {
                ++j;
                break;
            } else {
                ++j;
            }
        }
        j = std::min(j, n_);
        push(start, j, TokenKind::String);
        return j;
    }

    // Is s_[j] the sign of an exponent (1e-3, 0x1p+4) in the number starting at i?
    bool exponent_sign(int i, int j) const {
        const char prev = s_[j - 1];
        const bool hex = j - i >= 2 && s_[i] == '0' && (s_[i + 1] == 'x' || s_[i + 1] == 'X');
        return j > i && (prev == 'p' || prev == 'P' || (!hex && (prev == 'e' || prev == 'E')));
    }

    int number(int i) {
        int j = i;
        while (j < n_) {
            const unsigned char c = static_cast<unsigned char>(s_[j]);
            if (is_ident(c) || c == '.') {
                ++j;
            } else if (c == '\'' && j + 1 < n_ && is_ident(static_cast<unsigned char>(s_[j + 1]))) {
                j += 2;  // digit separator
            } else if ((c == '+' || c == '-') && exponent_sign(i, j)) {
                ++j;
            } else {
                break;
            }
        }
        push(i, j, TokenKind::Number);
        return j;
    }

    // "#  directive" at `hash`; returns the index after the directive name.
    int directive(int hash) {
        int j = hash + 1;
        while (j < n_ && is_blank(s_[j])) ++j;
        const int w = j;
        while (j < n_ && is_ident(static_cast<unsigned char>(s_[j]))) ++j;
        const std::string_view name = s_.substr(w, j - w);
        push(hash, j, TokenKind::Preprocessor);
        include_ = name == "include" || name == "include_next" || name == "import";
        define_ = name == "define" || name == "undef" || name == "ifdef" || name == "ifndef";
        pragma_ = name == "pragma";
        return j;
    }

    int identifier(int i) {
        int j = i;
        while (j < n_ && is_ident(static_cast<unsigned char>(s_[j]))) ++j;
        const std::string_view w = s_.substr(i, j - i);

        // Encoding prefixes: u8"..", L'..', R"(..)", u8R"x(..)x"
        if (j < n_ && (s_[j] == '"' || s_[j] == '\'') && is_string_prefix(w)) {
            return quoted(i, j, w.back() == 'R');
        }

        int k = j;
        while (k < n_ && is_blank(s_[k])) ++k;
        const char next = k < n_ ? s_[k] : '\0';

        TokenKind kind = TokenKind::Text;
        if (define_) {
            kind = TokenKind::Macro;
            define_ = false;
        } else if (pragma_ || w == "defined" || w == "__has_include") {
            kind = TokenKind::Preprocessor;
        } else if (contains(kControl, w)) {
            kind = TokenKind::Control;
        } else if (contains(kKeywords, w)) {
            kind = TokenKind::Keyword;
        } else if (contains(kTypes, w)) {
            kind = TokenKind::Type;
        } else if (is_macro_name(w)) {
            kind = TokenKind::Macro;
        } else if (type_next_ || is_pascal_case(w)) {
            kind = TokenKind::Type;
        } else if (next == '(') {
            kind = TokenKind::Function;
        }
        type_next_ = kind == TokenKind::Keyword && contains(kTypeIntroducers, w);
        push(i, j, kind);
        return j;
    }

    std::string_view s_;
    int n_;
    LexState& st_;
    std::vector<TokenSpan>& out_;
    bool include_ = false;   // inside #include: <...> is a string
    bool define_ = false;    // next identifier is the macro name
    bool pragma_ = false;    // #pragma: the rest is directive text
    bool type_next_ = false; // after class/struct/enum/namespace/...
};

} // namespace

void lex_cpp_line(std::string_view line, LexState& state, std::vector<TokenSpan>& out) {
    LineLexer(line, state, out).run();
}

bool CppHighlighter::update(const TextBuffer& buf) {
    if (valid_ && version_ == buf.version()) return false;
    version_ = buf.version();
    valid_ = true;
    last_relexed_ = 0;

    const int n = buf.line_count();
    const int m = static_cast<int>(lines_.size());
    int prefix = 0;
    while (prefix < std::min(n, m) && lines_[prefix].text == buf.line(prefix)) ++prefix;
    if (prefix == n && prefix == m) return false;
    int suffix = 0;
    while (suffix < std::min(n, m) - prefix && lines_[m - 1 - suffix].text == buf.line(n - 1 - suffix)) ++suffix;

    std::vector<Line> next;
    next.reserve(static_cast<size_t>(n));
    for (int k = 0; k < prefix; ++k) next.push_back(std::move(lines_[k]));
    LexState state = prefix > 0 ? next.back().out : LexState{};
    for (int i = prefix; i < n; ++i) {
        if (i >= n - suffix) {
            // Unchanged tail: reuse the cache once the carried state agrees.
            const int j = i - n + m;
            if (lines_[j].in == state) {
                for (int k = j; k < m; ++k) next.push_back(std::move(lines_[k]));
                break;
            }
        }
        Line line;
        line.text = buf.line(i);
        line.in = state;
        if (!plain_) lex_cpp_line(line.text, state, line.spans);
        line.out = state;
        next.push_back(std::move(line));
        ++last_relexed_;
    }
    lines_ = std::move(next);
    return last_relexed_ > 0;
}

const std::vector<TokenSpan>& CppHighlighter::spans(int line) const {
    static const std::vector<TokenSpan> kEmpty;
    if (line < 0 || line >= static_cast<int>(lines_.size())) return kEmpty;
    return lines_[line].spans;
}

LexState CppHighlighter::state_at(int line) const {
    if (line < 0 || line >= static_cast<int>(lines_.size())) return {};
    return lines_[line].in;
}

void CppHighlighter::set_plain_text(bool plain) {
    if (plain == plain_) return;
    plain_ = plain;
    clear();
}

void CppHighlighter::clear() {
    lines_.clear();
    valid_ = false;
    last_relexed_ = 0;
}

} // namespace osef::editor
