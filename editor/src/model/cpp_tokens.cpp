#include "model/cpp_tokens.h"

#include <cstdint>

namespace osef::editor::cpp {

namespace {

bool is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}
bool is_ident_char(unsigned char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alnum(unsigned char c) { return is_ident_char(c) && c < 0x80; }
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Is `s[i..]` a backslash-newline (LF or CRLF)? Returns its length, else 0.
size_t line_splice(std::string_view s, size_t i) {
    if (i >= s.size() || s[i] != '\\') return 0;
    if (i + 1 < s.size() && s[i + 1] == '\n') return 2;
    if (i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') return 3;
    return 0;
}

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    Tokens run() {
        Tokens out;
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                ++i_;
                continue;
            }
            if (size_t n = line_splice(s_, i_)) {
                i_ += n;
                continue;
            }
            if (c == '/' && peek(1) == '/') {
                size_t b = i_;
                skip_line_comment();
                out.comments.push_back({b, i_});
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                size_t b = i_;
                size_t close = s_.find("*/", i_ + 2);
                i_ = close == std::string_view::npos ? s_.size() : close + 2;
                out.comments.push_back({b, i_});
                continue;
            }
            if (c == '"') {
                out.tokens.push_back(string_literal(i_, i_));
                continue;
            }
            if (c == '\'') {
                out.tokens.push_back(char_literal(i_, i_));
                continue;
            }
            if (is_digit(c) || (c == '.' && is_digit(static_cast<unsigned char>(peek(1))))) {
                out.tokens.push_back(number());
                continue;
            }
            if (is_ident_start(c)) {
                out.tokens.push_back(identifier_or_literal());
                continue;
            }
            Token t;
            t.kind = TokKind::Punct;
            t.begin = i_;
            size_t len = 1;
            if ((c == ':' && peek(1) == ':') || (c == '-' && peek(1) == '>')) len = 2;
            i_ += len;
            t.end = i_;
            t.text = s_.substr(t.begin, len);
            out.tokens.push_back(std::move(t));
        }
        return out;
    }

private:
    char peek(size_t ahead) const { return i_ + ahead < s_.size() ? s_[i_ + ahead] : '\0'; }

    void skip_line_comment() {
        while (i_ < s_.size() && s_[i_] != '\n') {
            if (size_t n = line_splice(s_, i_)) {
                i_ += n;  // a backslash-newline continues the comment
                continue;
            }
            ++i_;
        }
    }

    Token finish(Token t) {
        t.end = i_;
        t.text = s_.substr(t.begin, t.end - t.begin);
        return t;
    }

    // `quote` is the offset of the opening '"'; `begin` the token start (prefix).
    Token string_literal(size_t begin, size_t quote) {
        Token t;
        t.kind = TokKind::String;
        t.begin = begin;
        i_ = quote + 1;
        size_t body = i_;
        bool closed = false;
        while (i_ < s_.size()) {
            char ch = s_[i_];
            if (ch == '\\' && i_ + 1 < s_.size()) {
                i_ += 2;
                continue;
            }
            if (ch == '"') {
                closed = true;
                break;
            }
            if (ch == '\n') break;  // unterminated: stop at the end of the line
            ++i_;
        }
        std::string_view inner = s_.substr(body, i_ - body);
        if (closed) ++i_;
        t.ok = decode_escapes(inner, t.value) && closed;
        return finish(std::move(t));
    }

    Token raw_string(size_t begin, size_t quote) {
        Token t;
        t.kind = TokKind::String;
        t.begin = begin;
        i_ = quote + 1;
        size_t open = s_.find('(', i_);
        if (open == std::string_view::npos || open - i_ > 16) {
            t.ok = false;
            i_ = s_.find('\n', i_);
            if (i_ == std::string_view::npos) i_ = s_.size();
            return finish(std::move(t));
        }
        std::string closer = ")";
        closer += s_.substr(i_, open - i_);
        closer += '"';
        size_t close = s_.find(closer, open + 1);
        if (close == std::string_view::npos) {
            t.ok = false;
            t.value = std::string(s_.substr(open + 1));
            i_ = s_.size();
            return finish(std::move(t));
        }
        t.value = std::string(s_.substr(open + 1, close - open - 1));
        i_ = close + closer.size();
        return finish(std::move(t));
    }

    Token char_literal(size_t begin, size_t quote) {
        Token t;
        t.kind = TokKind::Char;
        t.begin = begin;
        i_ = quote + 1;
        bool closed = false;
        while (i_ < s_.size()) {
            char ch = s_[i_];
            if (ch == '\\' && i_ + 1 < s_.size()) {
                i_ += 2;
                continue;
            }
            if (ch == '\'') {
                closed = true;
                ++i_;
                break;
            }
            if (ch == '\n') break;
            ++i_;
        }
        t.ok = closed;
        return finish(std::move(t));
    }

    Token number() {
        Token t;
        t.kind = TokKind::Number;
        t.begin = i_;
        ++i_;
        while (i_ < s_.size()) {
            unsigned char ch = static_cast<unsigned char>(s_[i_]);
            if ((ch == '+' || ch == '-') &&
                (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E' || s_[i_ - 1] == 'p' || s_[i_ - 1] == 'P')) {
                ++i_;
            } else if (ch == '\'' && i_ + 1 < s_.size() && is_alnum(static_cast<unsigned char>(s_[i_ + 1]))) {
                i_ += 2;  // digit separator
            } else if (is_alnum(ch) || ch == '.' || ch == '_') {
                ++i_;
            } else {
                break;
            }
        }
        return finish(std::move(t));
    }

    Token identifier_or_literal() {
        size_t b = i_;
        while (i_ < s_.size() && is_ident_char(static_cast<unsigned char>(s_[i_]))) ++i_;
        std::string_view word = s_.substr(b, i_ - b);
        char next = i_ < s_.size() ? s_[i_] : '\0';
        if (next == '"') {
            if (word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R")
                return raw_string(b, i_);
            if (word == "L" || word == "u" || word == "U" || word == "u8") return string_literal(b, i_);
        }
        if (next == '\'' && (word == "L" || word == "u" || word == "U" || word == "u8"))
            return char_literal(b, i_);
        Token t;
        t.kind = TokKind::Identifier;
        t.begin = b;
        return finish(std::move(t));
    }

    std::string_view s_;
    size_t i_ = 0;
};

} // namespace

bool decode_escapes(std::string_view body, std::string& out) {
    bool ok = true;
    out.clear();
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (size_t n = line_splice(body, i)) {
            i += n - 1;  // line continuation inside a string
            continue;
        }
        if (i + 1 >= body.size()) {
            ok = false;
            break;
        }
        char e = body[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case '\\': out += '\\'; break;
        case '\'': out += '\''; break;
        case '"': out += '"'; break;
        case '?': out += '?'; break;
        case 'x': {
            uint32_t v = 0;
            size_t digits = 0;
            while (i + 1 < body.size() && hex_value(body[i + 1]) >= 0) {
                v = v * 16 + static_cast<uint32_t>(hex_value(body[++i]));
                if (v > 0xFF) ok = false;
                ++digits;
            }
            if (digits == 0) ok = false;
            out += static_cast<char>(v & 0xFF);
            break;
        }
        case 'u':
        case 'U': {
            size_t want = e == 'u' ? 4 : 8;
            uint32_t v = 0;
            size_t digits = 0;
            while (digits < want && i + 1 < body.size() && hex_value(body[i + 1]) >= 0) {
                v = v * 16 + static_cast<uint32_t>(hex_value(body[++i]));
                ++digits;
            }
            if (digits != want || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
                ok = false;
                break;
            }
            append_utf8(out, v);
            break;
        }
        default:
            if (e >= '0' && e <= '7') {
                uint32_t v = static_cast<uint32_t>(e - '0');
                for (int k = 0; k < 2 && i + 1 < body.size() && body[i + 1] >= '0' && body[i + 1] <= '7'; ++k)
                    v = v * 8 + static_cast<uint32_t>(body[++i] - '0');
                if (v > 0xFF) ok = false;
                out += static_cast<char>(v & 0xFF);
            } else {
                ok = false;  // unknown escape
                out += e;
            }
            break;
        }
    }
    return ok;
}

Tokens tokenize(std::string_view source) { return Lexer(source).run(); }

size_t match_bracket(const std::vector<Token>& tokens, size_t open) {
    if (open >= tokens.size() || tokens[open].kind != TokKind::Punct) return npos;
    const std::string_view o = tokens[open].text;
    const std::string_view c = o == "(" ? ")" : o == "[" ? "]" : o == "{" ? "}" : "";
    if (c.empty()) return npos;
    int depth = 0;
    for (size_t i = open; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.kind != TokKind::Punct) continue;
        if (t.text == o) {
            ++depth;
        } else if (t.text == c) {
            if (--depth == 0) return i;
        }
    }
    return npos;
}

namespace {

// Is there a line break in source[from, to) outside comments and splices?
bool line_break_between(std::string_view s, const std::vector<Range>& comments, size_t& ci, size_t from, size_t to) {
    size_t p = from;
    while (p < to) {
        while (ci < comments.size() && comments[ci].end <= p) ++ci;
        if (ci < comments.size() && comments[ci].begin <= p) {
            p = comments[ci].end;
            continue;
        }
        if (size_t n = line_splice(s, p)) {
            p += n;
            continue;
        }
        if (s[p] == '\n') return true;
        ++p;
    }
    return false;
}

enum class Branch { Active, Inactive, Unknown };

struct CondFrame {
    Branch branch = Branch::Active;
    bool taken = false;         // some branch is known to be active
    bool unknown_seen = false;  // some earlier branch could not be evaluated
};

// Value of a simple #if/#elif condition: 0/1/true/false, else Unknown.
Branch eval_condition(const std::vector<Token>& t, size_t a, size_t b) {
    if (b != a + 1) return Branch::Unknown;
    const std::string_view v = t[a].text;
    if (v == "0" || v == "false") return Branch::Inactive;
    if (v == "1" || v == "true") return Branch::Active;
    return Branch::Unknown;
}

} // namespace

ActiveCode active_code(std::string_view source, const Tokens& toks) {
    ActiveCode out;
    const std::vector<Token>& t = toks.tokens;
    std::vector<CondFrame> stack;
    size_t ci = 0;
    auto line_start = [&](size_t i) {
        if (i == 0) {
            // Only whitespace (or comments) before the first token.
            size_t c = 0;
            return !line_break_between(source, toks.comments, c, 0, 0) &&
                   source.substr(0, t[0].begin).find_first_not_of(" \t\r\n\f\v") == std::string_view::npos ||
                   source.substr(0, t[0].begin).find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
        }
        size_t c = ci;
        return line_break_between(source, toks.comments, c, t[i - 1].end, t[i].begin);
    };
    auto active = [&]() {
        for (const CondFrame& f : stack)
            if (f.branch == Branch::Inactive) return false;
        return true;
    };
    for (size_t i = 0; i < t.size();) {
        if (t[i].is("#") && line_start(i)) {
            // The directive runs to the end of its (spliced) line.
            size_t j = i + 1;
            size_t cj = ci;
            while (j < t.size() && !line_break_between(source, toks.comments, cj, t[j - 1].end, t[j].begin)) ++j;
            ci = cj;
            out.directives.push_back(Range{t[i].begin, t[j - 1].end});
            const std::string_view name = j > i + 1 ? t[i + 1].text : std::string_view();
            if (name == "if" || name == "ifdef" || name == "ifndef") {
                CondFrame f;
                f.branch = name == "if" ? eval_condition(t, i + 2, j) : Branch::Unknown;
                f.taken = f.branch == Branch::Active;
                f.unknown_seen = f.branch == Branch::Unknown;
                stack.push_back(f);
            } else if ((name == "elif" || name == "elifdef" || name == "elifndef" || name == "else") && !stack.empty()) {
                CondFrame& f = stack.back();
                const Branch v = name == "else" ? Branch::Active : name == "elif" ? eval_condition(t, i + 2, j)
                                                                                    : Branch::Unknown;
                if (f.taken) {
                    f.branch = Branch::Inactive;
                } else if (v == Branch::Inactive) {
                    f.branch = Branch::Inactive;
                } else if (v == Branch::Active && !f.unknown_seen) {
                    f.branch = Branch::Active;
                    f.taken = true;
                } else {
                    f.branch = Branch::Unknown;
                    f.unknown_seen = true;
                }
            } else if (name == "endif" && !stack.empty()) {
                stack.pop_back();
            }
            i = j;
            continue;
        }
        if (active()) {
            for (const CondFrame& f : stack)
                if (f.branch == Branch::Unknown) out.uncertain = true;
            out.tokens.push_back(t[i]);
        }
        ++i;
    }
    return out;
}

int line_of(std::string_view source, size_t offset) {
    if (offset > source.size()) offset = source.size();
    int line = 1;
    for (size_t i = 0; i < offset; ++i)
        if (source[i] == '\n') ++line;
    return line;
}

} // namespace osef::editor::cpp
