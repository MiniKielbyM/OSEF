// TextBuffer: lines of UTF-8 text, cursor + selection, undo/redo, find.
//
// Every edit is recorded as a list of Ops (replace `removed` at `pos` with
// `inserted`), applied in order and undone in reverse. Each undo step has a
// unique id; modified() compares the id on top of the undo stack with the id
// at the last save, so undoing back to the saved state clears it.

#include "text/text_buffer.h"

#include <algorithm>
#include <utility>

namespace osef::editor {

namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Identifier characters; non-ASCII bytes count as word characters so
// multibyte letters move and select as part of words.
bool is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c >= 0x80;
}

int next_cp(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    if (col >= n) return n;
    ++col;
    while (col < n && is_cont(static_cast<unsigned char>(s[col]))) ++col;
    return col;
}

int prev_cp(const std::string& s, int col) {
    if (col <= 0) return 0;
    --col;
    while (col > 0 && is_cont(static_cast<unsigned char>(s[col]))) --col;
    return col;
}

// Clamps to [0, size] and backs off to a code point boundary.
int fix_col(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    col = std::clamp(col, 0, n);
    while (col > 0 && col < n && is_cont(static_cast<unsigned char>(s[col]))) --col;
    return col;
}

int leading_blank(const std::string& s) {
    int i = 0;
    while (i < static_cast<int>(s.size()) && is_blank(s[i])) ++i;
    return i;
}

bool all_blank(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return is_blank(c); });
}

// "\r\n" and lone "\r" become "\n".
std::string normalize_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Byte index of `needle` in `hay` at or after `from`, or npos.
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

bool is_single_code_point(std::string_view s) {
    if (s.empty()) return false;
    size_t n = 1;
    while (n < s.size() && is_cont(static_cast<unsigned char>(s[n]))) ++n;
    return n == s.size();
}

enum class MergeKind { None, Typing, Backspace, Delete };

struct Op {
    TextPos pos;
    std::string removed;
    std::string inserted;
};

struct Edit {
    std::vector<Op> ops;
    TextPos cursor_before, anchor_before, cursor_after, anchor_after;
    uint64_t id = 0;
    MergeKind kind = MergeKind::None;
};

// End position of `text` inserted at `p`.
TextPos advance(TextPos p, std::string_view text) {
    for (char c : text) {
        if (c == '\n') {
            ++p.line;
            p.col = 0;
        } else {
            ++p.col;
        }
    }
    return p;
}

} // namespace

struct TextBuffer::Impl {
    std::vector<std::string> lines{std::string()};
    TextPos cursor, anchor;
    int preferred_vcol = -1;  // sticky display column for up/down, -1 = none
    uint64_t version = 0;
    int tab_size = 4;

    std::vector<Edit> undo, redo;
    uint64_t next_id = 1;
    uint64_t saved_id = 0;
    bool merge_blocked = true;
    Edit pending;

    int count() const { return static_cast<int>(lines.size()); }
    int len(int l) const { return static_cast<int>(lines[l].size()); }
    uint64_t current_id() const { return undo.empty() ? 0 : undo.back().id; }
    TextPos end_pos() const { return {count() - 1, len(count() - 1)}; }
    bool has_sel() const { return cursor != anchor; }
    TextPos sel_begin() const { return std::min(cursor, anchor); }
    TextPos sel_end() const { return std::max(cursor, anchor); }

    TextPos clamp(TextPos p) const {
        if (p.line < 0) return {0, 0};
        if (p.line >= count()) return end_pos();
        return {p.line, fix_col(lines[p.line], p.col)};
    }

    void load(std::string_view text) {
        const std::string norm = normalize_newlines(text);
        lines.clear();
        size_t start = 0;
        while (true) {
            const size_t nl = norm.find('\n', start);
            if (nl == std::string::npos) {
                lines.emplace_back(norm.substr(start));
                break;
            }
            lines.emplace_back(norm.substr(start, nl - start));
            start = nl + 1;
        }
        cursor = anchor = {};
        preferred_vcol = -1;
        undo.clear();
        redo.clear();
        saved_id = 0;
        merge_blocked = true;
        ++version;
    }

    std::string join() const {
        size_t total = lines.size();
        for (const auto& l : lines) total += l.size();
        std::string out;
        out.reserve(total);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out.push_back('\n');
            out += lines[i];
        }
        return out;
    }

    std::string get_range(TextPos a, TextPos b) const {
        if (b <= a) return {};
        if (a.line == b.line) return lines[a.line].substr(a.col, b.col - a.col);
        std::string out = lines[a.line].substr(a.col);
        for (int l = a.line + 1; l < b.line; ++l) {
            out.push_back('\n');
            out += lines[l];
        }
        out.push_back('\n');
        out += lines[b.line].substr(0, b.col);
        return out;
    }

    void raw_erase(TextPos a, TextPos b) {
        if (b <= a) return;
        if (a.line == b.line) {
            lines[a.line].erase(a.col, b.col - a.col);
            return;
        }
        lines[a.line] = lines[a.line].substr(0, a.col) + lines[b.line].substr(b.col);
        lines.erase(lines.begin() + a.line + 1, lines.begin() + b.line + 1);
    }

    TextPos raw_insert(TextPos p, std::string_view text) {
        if (text.empty()) return p;
        const size_t nl = text.find('\n');
        if (nl == std::string_view::npos) {
            lines[p.line].insert(p.col, text);
            return {p.line, p.col + static_cast<int>(text.size())};
        }
        std::string tail = lines[p.line].substr(p.col);
        lines[p.line].erase(p.col);
        lines[p.line].append(text.substr(0, nl));
        std::vector<std::string> added;
        size_t start = nl + 1;
        while (true) {
            const size_t next = text.find('\n', start);
            if (next == std::string_view::npos) {
                added.emplace_back(text.substr(start));
                break;
            }
            added.emplace_back(text.substr(start, next - start));
            start = next + 1;
        }
        TextPos end{p.line + static_cast<int>(added.size()), static_cast<int>(added.back().size())};
        added.back() += tail;
        lines.insert(lines.begin() + p.line + 1, std::make_move_iterator(added.begin()),
                     std::make_move_iterator(added.end()));
        return end;
    }

    // ---- transactions ----

    void begin(MergeKind kind = MergeKind::None) {
        pending = Edit{};
        pending.cursor_before = cursor;
        pending.anchor_before = anchor;
        pending.kind = kind;
    }

    // Replaces [a, b) with `text` inside the pending edit; returns the end of `text`.
    TextPos replace(TextPos a, TextPos b, std::string_view text) {
        Op op{a, get_range(a, b), std::string(text)};
        if (op.removed.empty() && op.inserted.empty()) return a;
        raw_erase(a, b);
        const TextPos end = raw_insert(a, text);
        pending.ops.push_back(std::move(op));
        return end;
    }

    void commit() {
        preferred_vcol = -1;
        if (pending.ops.empty()) {
            merge_blocked = true;
            return;
        }
        pending.cursor_after = cursor;
        pending.anchor_after = anchor;
        pending.id = next_id++;
        redo.clear();
        undo.push_back(std::move(pending));
        pending = Edit{};
        merge_blocked = undo.back().kind == MergeKind::None;
        ++version;
    }

    // The top undo step that a new keystroke of `kind` may extend, or nullptr.
    Edit* mergeable(MergeKind kind) {
        if (merge_blocked || undo.empty() || has_sel()) return nullptr;
        Edit& top = undo.back();
        if (top.kind != kind || top.id == saved_id || top.ops.size() != 1) return nullptr;
        if (top.cursor_after != cursor || top.anchor_after != anchor) return nullptr;
        return &top;
    }

    void apply(const Op& op, bool forward) {
        const std::string& out = forward ? op.removed : op.inserted;
        const std::string& in = forward ? op.inserted : op.removed;
        raw_erase(op.pos, advance(op.pos, out));
        raw_insert(op.pos, in);
    }

    // ---- line ranges ----

    // Lines touched by the selection; a selection ending at column 0 of a
    // later line doesn't include that line.
    void selected_lines(int& first, int& last) const {
        const TextPos b = sel_begin(), e = sel_end();
        first = b.line;
        last = e.line;
        if (e.line > b.line && e.col == 0) --last;
    }

    // Shift a position after `n` bytes were inserted (n > 0) or removed (n < 0) at (line, col).
    static void shift(TextPos& p, int line, int col, int n, bool inclusive) {
        if (p.line != line) return;
        if (n >= 0) {
            if (p.col > col || (inclusive && p.col == col)) p.col += n;
        } else if (p.col > col) {
            p.col = std::max(col, p.col + n);
        }
    }

    // ---- movement helpers ----

    void place(TextPos p, bool extend) {
        cursor = p;
        if (!extend) anchor = p;
        merge_blocked = true;
    }

    TextPos left_of(TextPos p, bool word) const {
        if (p.col == 0) return p.line > 0 ? TextPos{p.line - 1, len(p.line - 1)} : p;
        const std::string& s = lines[p.line];
        if (!word) return {p.line, prev_cp(s, p.col)};
        int c = p.col;
        while (c > 0 && is_blank(s[c - 1])) --c;
        if (c == 0) return {p.line, 0};
        const bool w = is_word(static_cast<unsigned char>(s[c - 1]));
        while (c > 0 && !is_blank(s[c - 1]) && is_word(static_cast<unsigned char>(s[c - 1])) == w) --c;
        return {p.line, c};
    }

    TextPos right_of(TextPos p, bool word) const {
        const std::string& s = lines[p.line];
        const int n = len(p.line);
        if (p.col >= n) return p.line + 1 < count() ? TextPos{p.line + 1, 0} : p;
        if (!word) return {p.line, next_cp(s, p.col)};
        int c = p.col;
        while (c < n && is_blank(s[c])) ++c;
        if (c == n) return {p.line, n};
        const bool w = is_word(static_cast<unsigned char>(s[c]));
        while (c < n && !is_blank(s[c]) && is_word(static_cast<unsigned char>(s[c])) == w) ++c;
        return {p.line, c};
    }

    int visual_column(int l, int col) const {
        const std::string& s = lines[l];
        col = std::min(col, len(l));
        int v = 0;
        for (int i = 0; i < col; i = next_cp(s, i)) v = s[i] == '\t' ? v + tab_size - v % tab_size : v + 1;
        return v;
    }

    int column_at_visual(int l, float vcol) const {
        const std::string& s = lines[l];
        int v = 0;
        for (int i = 0; i < len(l); i = next_cp(s, i)) {
            const int w = s[i] == '\t' ? tab_size - v % tab_size : 1;
            if (vcol < static_cast<float>(v) + static_cast<float>(w) * 0.5f) return i;
            v += w;
        }
        return len(l);
    }

    void vertical(int delta, bool extend) {
        if (preferred_vcol < 0) preferred_vcol = visual_column(cursor.line, cursor.col);
        const int vcol = preferred_vcol;
        const int target = cursor.line + delta;
        if (target < 0) {
            place({0, 0}, extend);
        } else if (target >= count()) {
            place(end_pos(), extend);
        } else {
            place({target, column_at_visual(target, static_cast<float>(vcol))}, extend);
        }
        preferred_vcol = vcol;
    }
};

// ---- construction / content ---------------------------------------------------

TextBuffer::TextBuffer() : impl_(new Impl) {}
TextBuffer::TextBuffer(std::string_view text) : impl_(new Impl) { impl_->load(text); }
TextBuffer::~TextBuffer() { delete impl_; }

std::string TextBuffer::text() const { return impl_->join(); }
void TextBuffer::set_text(std::string_view text) { impl_->load(text); }
int TextBuffer::line_count() const { return impl_->count(); }
const std::string& TextBuffer::line(int index) const {
    return impl_->lines[std::clamp(index, 0, impl_->count() - 1)];
}
uint64_t TextBuffer::version() const { return impl_->version; }

// ---- cursor and selection -----------------------------------------------------

TextPos TextBuffer::cursor() const { return impl_->cursor; }
TextPos TextBuffer::anchor() const { return impl_->anchor; }
bool TextBuffer::has_selection() const { return impl_->has_sel(); }
TextPos TextBuffer::selection_begin() const { return impl_->sel_begin(); }
TextPos TextBuffer::selection_end() const { return impl_->sel_end(); }
std::string TextBuffer::selected_text() const { return impl_->get_range(impl_->sel_begin(), impl_->sel_end()); }
TextPos TextBuffer::clamp(TextPos pos) const { return impl_->clamp(pos); }

void TextBuffer::set_cursor(TextPos pos, bool extend_selection) {
    impl_->place(impl_->clamp(pos), extend_selection);
    impl_->preferred_vcol = -1;
}

void TextBuffer::set_selection(TextPos anchor, TextPos cursor) {
    impl_->anchor = impl_->clamp(anchor);
    impl_->cursor = impl_->clamp(cursor);
    impl_->merge_blocked = true;
    impl_->preferred_vcol = -1;
}

void TextBuffer::select_all() { set_selection({0, 0}, impl_->end_pos()); }
void TextBuffer::clear_selection() { set_cursor(impl_->cursor); }

void TextBuffer::select_word_at(TextPos pos) {
    const TextPos p = impl_->clamp(pos);
    const std::string& s = impl_->lines[p.line];
    const int n = static_cast<int>(s.size());
    int at = -1;
    if (p.col < n && is_word(static_cast<unsigned char>(s[p.col])))
        at = p.col;
    else if (p.col > 0 && is_word(static_cast<unsigned char>(s[p.col - 1])))
        at = p.col - 1;
    if (at < 0) {
        if (p.col < n)
            set_selection(p, {p.line, next_cp(s, p.col)});
        else
            set_cursor(p);
        return;
    }
    int b = at, e = at;
    while (b > 0 && is_word(static_cast<unsigned char>(s[b - 1]))) --b;
    while (e < n && is_word(static_cast<unsigned char>(s[e]))) ++e;
    set_selection({p.line, b}, {p.line, e});
}

void TextBuffer::select_line(int line) {
    const int l = std::clamp(line, 0, impl_->count() - 1);
    if (l + 1 < impl_->count())
        set_selection({l, 0}, {l + 1, 0});
    else
        set_selection({l, 0}, {l, impl_->len(l)});
}

// ---- editing --------------------------------------------------------------------

void TextBuffer::insert(std::string_view raw) {
    Impl& m = *impl_;
    const std::string text = normalize_newlines(raw);
    if (text.empty() && !m.has_sel()) return;

    const bool typing = is_single_code_point(text) && text != "\n";
    if (typing) {
        if (Edit* top = m.mergeable(MergeKind::Typing)) {
            Op& op = top->ops.front();
            const bool same_line = op.pos.line == m.cursor.line && op.inserted.find('\n') == std::string::npos;
            const bool word_starts = !op.inserted.empty() && is_blank(op.inserted.back()) && !is_blank(text[0]);
            if (same_line && !word_starts && advance(op.pos, op.inserted) == m.cursor && text != "}") {
                m.cursor = m.anchor = m.raw_insert(m.cursor, text);
                op.inserted += text;
                top->cursor_after = top->anchor_after = m.cursor;
                m.preferred_vcol = -1;
                ++m.version;
                return;
            }
        }
    }

    m.begin(typing ? MergeKind::Typing : MergeKind::None);
    TextPos b = m.sel_begin();
    const TextPos e = m.sel_end();
    // Typing '}' on a blank-so-far line removes one indent level first.
    if (text == "}" && b == e && b.col > 0 && all_blank(std::string_view(m.lines[b.line]).substr(0, b.col))) {
        // Back to the previous tab stop.
        const int target = (m.visual_column(b.line, b.col) - 1) / m.tab_size * m.tab_size;
        const int keep = m.column_at_visual(b.line, static_cast<float>(target));
        if (keep < b.col) {
            m.pending.kind = MergeKind::None;
            m.replace({b.line, keep}, b, "");
            b.col = keep;
            m.cursor = m.anchor = m.replace(b, b, text);
            m.commit();
            return;
        }
    }
    m.cursor = m.anchor = m.replace(b, e, text);
    m.commit();
}

void TextBuffer::newline() {
    Impl& m = *impl_;
    m.begin();
    TextPos b = m.sel_begin();
    const TextPos e = m.sel_end();
    const std::string& l = m.lines[b.line];
    const int lead = std::min(leading_blank(l), b.col);
    const std::string indent = l.substr(0, lead);
    int k = b.col;
    while (k > 0 && is_blank(l[k - 1])) --k;
    const bool after_open = k > 0 && l[k - 1] == '{';
    const std::string& el = m.lines[e.line];
    int j = e.col;
    while (j < static_cast<int>(el.size()) && is_blank(el[j])) ++j;
    const bool before_close = j < static_cast<int>(el.size()) && el[j] == '}';
    const std::string unit(static_cast<size_t>(m.tab_size), ' ');

    // A line that is blank up to the cursor keeps no trailing whitespace.
    if (b.col > 0 && all_blank(std::string_view(l).substr(0, b.col))) b.col = 0;

    std::string text = "\n" + indent;
    if (after_open) text += unit;
    if (after_open && before_close) {
        const TextPos mid = m.replace(b, e, text);
        m.replace(mid, mid, "\n" + indent);
        m.cursor = m.anchor = mid;
    } else {
        m.cursor = m.anchor = m.replace(b, e, text);
    }
    m.commit();
}

void TextBuffer::backspace() {
    Impl& m = *impl_;
    if (m.has_sel()) {
        m.begin();
        m.cursor = m.anchor = m.replace(m.sel_begin(), m.sel_end(), "");
        m.commit();
        return;
    }
    const TextPos c = m.cursor;
    if (c.line == 0 && c.col == 0) return;
    TextPos from;
    const std::string& l = m.lines[c.line];
    if (c.col == 0) {
        from = {c.line - 1, m.len(c.line - 1)};
    } else if (all_blank(std::string_view(l).substr(0, c.col)) &&
               l.find('\t') >= static_cast<size_t>(c.col)) {
        // In leading spaces: back to the previous tab stop.
        const int step = (c.col - 1) % m.tab_size + 1;
        from = {c.line, c.col - step};
    } else {
        from = {c.line, prev_cp(l, c.col)};
    }
    if (Edit* top = m.mergeable(MergeKind::Backspace); top && from.line == c.line) {
        Op& op = top->ops.front();
        if (op.inserted.empty() && op.pos == c && op.removed.find('\n') == std::string::npos) {
            op.removed.insert(0, m.get_range(from, c));
            op.pos = from;
            m.raw_erase(from, c);
            m.cursor = m.anchor = from;
            top->cursor_after = top->anchor_after = from;
            ++m.version;
            m.preferred_vcol = -1;
            return;
        }
    }
    m.begin(from.line == c.line ? MergeKind::Backspace : MergeKind::None);
    m.cursor = m.anchor = m.replace(from, c, "");
    m.commit();
}

void TextBuffer::delete_forward() {
    Impl& m = *impl_;
    if (m.has_sel()) {
        m.begin();
        m.cursor = m.anchor = m.replace(m.sel_begin(), m.sel_end(), "");
        m.commit();
        return;
    }
    const TextPos c = m.cursor;
    const TextPos to = m.right_of(c, false);
    if (to == c) return;
    if (Edit* top = m.mergeable(MergeKind::Delete); top && to.line == c.line) {
        Op& op = top->ops.front();
        if (op.inserted.empty() && op.pos == c && op.removed.find('\n') == std::string::npos) {
            op.removed += m.get_range(c, to);
            m.raw_erase(c, to);
            ++m.version;
            m.preferred_vcol = -1;
            return;
        }
    }
    m.begin(to.line == c.line ? MergeKind::Delete : MergeKind::None);
    m.replace(c, to, "");
    m.cursor = m.anchor = c;
    m.commit();
}

void TextBuffer::indent() {
    Impl& m = *impl_;
    m.begin();
    if (!m.has_sel()) {
        const int v = m.visual_column(m.cursor.line, m.cursor.col);
        const std::string spaces(static_cast<size_t>(m.tab_size - v % m.tab_size), ' ');
        m.cursor = m.anchor = m.replace(m.cursor, m.cursor, spaces);
        m.commit();
        return;
    }
    int first = 0, last = 0;
    m.selected_lines(first, last);
    const std::string unit(static_cast<size_t>(m.tab_size), ' ');
    for (int l = first; l <= last; ++l) {
        if (m.lines[l].empty()) continue;
        m.replace({l, 0}, {l, 0}, unit);
        Impl::shift(m.cursor, l, 0, m.tab_size, false);
        Impl::shift(m.anchor, l, 0, m.tab_size, false);
    }
    m.commit();
}

void TextBuffer::unindent() {
    Impl& m = *impl_;
    m.begin();
    int first = 0, last = 0;
    m.selected_lines(first, last);
    for (int l = first; l <= last; ++l) {
        const std::string& s = m.lines[l];
        int k = 0;
        if (!s.empty() && s[0] == '\t') {
            k = 1;
        } else {
            while (k < m.tab_size && k < static_cast<int>(s.size()) && s[k] == ' ') ++k;
        }
        if (k == 0) continue;
        m.replace({l, 0}, {l, k}, "");
        Impl::shift(m.cursor, l, 0, -k, false);
        Impl::shift(m.anchor, l, 0, -k, false);
    }
    m.commit();
}

void TextBuffer::toggle_line_comment() {
    Impl& m = *impl_;
    int first = 0, last = 0;
    m.selected_lines(first, last);
    bool any = false, all_commented = true;
    int min_indent = 1 << 30;
    for (int l = first; l <= last; ++l) {
        const std::string& s = m.lines[l];
        const int lead = leading_blank(s);
        if (lead == static_cast<int>(s.size())) continue;
        any = true;
        min_indent = std::min(min_indent, lead);
        if (s.compare(lead, 2, "//") != 0) all_commented = false;
    }
    m.begin();
    const bool sel = m.has_sel();
    if (!any) {
        // Only blank lines: comment the cursor's line at its indentation.
        const int l = m.cursor.line;
        const int lead = leading_blank(m.lines[l]);
        m.replace({l, lead}, {l, lead}, "// ");
        Impl::shift(m.cursor, l, lead, 3, true);
        Impl::shift(m.anchor, l, lead, 3, true);
    } else if (all_commented) {
        for (int l = first; l <= last; ++l) {
            const std::string& s = m.lines[l];
            const int lead = leading_blank(s);
            if (lead == static_cast<int>(s.size())) continue;
            const int n = (lead + 2 < static_cast<int>(s.size()) && s[lead + 2] == ' ') ? 3 : 2;
            m.replace({l, lead}, {l, lead + n}, "");
            Impl::shift(m.cursor, l, lead, -n, false);
            Impl::shift(m.anchor, l, lead, -n, false);
        }
    } else {
        for (int l = first; l <= last; ++l) {
            const std::string& s = m.lines[l];
            if (leading_blank(s) == static_cast<int>(s.size())) continue;
            m.replace({l, min_indent}, {l, min_indent}, "// ");
            Impl::shift(m.cursor, l, min_indent, 3, !sel);
            Impl::shift(m.anchor, l, min_indent, 3, !sel);
        }
    }
    m.commit();
}

void TextBuffer::delete_line() {
    Impl& m = *impl_;
    int first = 0, last = 0;
    m.selected_lines(first, last);
    const int col = m.cursor.col;
    m.begin();
    if (last + 1 < m.count()) {
        m.replace({first, 0}, {last + 1, 0}, "");
    } else if (first > 0) {
        m.replace({first - 1, m.len(first - 1)}, {last, m.len(last)}, "");
    } else {
        m.replace({0, 0}, {last, m.len(last)}, "");
    }
    const int line = std::min(first, m.count() - 1);
    m.cursor = m.anchor = {line, fix_col(m.lines[line], col)};
    m.commit();
}

void TextBuffer::replace_all(std::string_view raw) {
    Impl& m = *impl_;
    const std::string next = normalize_newlines(raw);
    const std::string prev = m.join();
    if (next == prev) return;

    // Replace only the differing middle so the undo record stays small.
    size_t prefix = 0;
    const size_t max_common = std::min(prev.size(), next.size());
    while (prefix < max_common && prev[prefix] == next[prefix]) ++prefix;
    while (prefix > 0 && prefix < prev.size() && is_cont(static_cast<unsigned char>(prev[prefix]))) --prefix;
    size_t suffix = 0;
    while (suffix < max_common - prefix && prev[prev.size() - 1 - suffix] == next[next.size() - 1 - suffix]) ++suffix;
    while (suffix > 0 && is_cont(static_cast<unsigned char>(prev[prev.size() - suffix]))) --suffix;

    auto pos_of = [&](size_t offset) {
        TextPos p;
        for (size_t i = 0; i < offset; ++i) {
            if (prev[i] == '\n') {
                ++p.line;
                p.col = 0;
            } else {
                ++p.col;
            }
        }
        return p;
    };
    const TextPos keep = m.cursor;
    m.begin();
    m.replace(pos_of(prefix), pos_of(prev.size() - suffix),
              std::string_view(next).substr(prefix, next.size() - suffix - prefix));
    m.cursor = m.anchor = m.clamp(keep);
    m.commit();
    m.merge_blocked = true;
}

// ---- movement ---------------------------------------------------------------------

void TextBuffer::move_left(bool extend, bool word) {
    Impl& m = *impl_;
    if (m.has_sel() && !extend && !word)
        m.place(m.sel_begin(), false);
    else
        m.place(m.left_of(m.cursor, word), extend);
    m.preferred_vcol = -1;
}

void TextBuffer::move_right(bool extend, bool word) {
    Impl& m = *impl_;
    if (m.has_sel() && !extend && !word)
        m.place(m.sel_end(), false);
    else
        m.place(m.right_of(m.cursor, word), extend);
    m.preferred_vcol = -1;
}

void TextBuffer::move_up(bool extend) { impl_->vertical(-1, extend); }
void TextBuffer::move_down(bool extend) { impl_->vertical(1, extend); }
void TextBuffer::move_lines(int delta, bool extend) { impl_->vertical(delta, extend); }

void TextBuffer::move_home(bool extend) {
    Impl& m = *impl_;
    const int first = leading_blank(m.lines[m.cursor.line]);
    m.place({m.cursor.line, m.cursor.col == first ? 0 : first}, extend);
    m.preferred_vcol = -1;
}

void TextBuffer::move_end(bool extend) {
    Impl& m = *impl_;
    m.place({m.cursor.line, m.len(m.cursor.line)}, extend);
    m.preferred_vcol = -1;
}

void TextBuffer::move_doc_start(bool extend) {
    impl_->place({0, 0}, extend);
    impl_->preferred_vcol = -1;
}

void TextBuffer::move_doc_end(bool extend) {
    impl_->place(impl_->end_pos(), extend);
    impl_->preferred_vcol = -1;
}

// ---- undo/redo --------------------------------------------------------------------

bool TextBuffer::can_undo() const { return !impl_->undo.empty(); }
bool TextBuffer::can_redo() const { return !impl_->redo.empty(); }

bool TextBuffer::undo() {
    Impl& m = *impl_;
    if (m.undo.empty()) return false;
    Edit e = std::move(m.undo.back());
    m.undo.pop_back();
    for (auto it = e.ops.rbegin(); it != e.ops.rend(); ++it) m.apply(*it, false);
    m.cursor = m.clamp(e.cursor_before);
    m.anchor = m.clamp(e.anchor_before);
    m.redo.push_back(std::move(e));
    m.merge_blocked = true;
    m.preferred_vcol = -1;
    ++m.version;
    return true;
}

bool TextBuffer::redo() {
    Impl& m = *impl_;
    if (m.redo.empty()) return false;
    Edit e = std::move(m.redo.back());
    m.redo.pop_back();
    for (const Op& op : e.ops) m.apply(op, true);
    m.cursor = m.clamp(e.cursor_after);
    m.anchor = m.clamp(e.anchor_after);
    m.undo.push_back(std::move(e));
    m.merge_blocked = true;
    m.preferred_vcol = -1;
    ++m.version;
    return true;
}

void TextBuffer::break_undo_group() { impl_->merge_blocked = true; }
size_t TextBuffer::undo_depth() const { return impl_->undo.size(); }

// ---- find ---------------------------------------------------------------------------

bool TextBuffer::find(std::string_view needle, bool forward, bool case_sensitive, bool wrap) {
    Impl& m = *impl_;
    if (needle.empty() || needle.find('\n') != std::string_view::npos) return false;
    const int n = m.count();
    const int nlen = static_cast<int>(needle.size());
    auto select = [&](int line, size_t at) {
        set_selection({line, static_cast<int>(at)}, {line, static_cast<int>(at) + nlen});
        return true;
    };

    if (forward) {
        // Continue after the current match, otherwise search from the selection start.
        const std::string sel = selected_text();
        const bool on_match = m.has_sel() && find_in(sel, needle, 0, case_sensitive) == 0 &&
                              static_cast<int>(sel.size()) == nlen;
        const TextPos start = on_match ? m.sel_end() : m.sel_begin();
        for (int i = 0; i <= n; ++i) {
            const int l = (start.line + i) % n;
            if (!wrap && start.line + i >= n) break;
            const std::string& s = m.lines[l];
            size_t from = 0;
            if (i == 0) from = static_cast<size_t>(start.col);
            size_t at = find_in(s, needle, from, case_sensitive);
            // On the wrapped-around start line only matches before the start count.
            if (i == n && at != std::string_view::npos && static_cast<int>(at) >= start.col) at = std::string_view::npos;
            if (at != std::string_view::npos) return select(l, at);
        }
        return false;
    }

    const TextPos start = m.sel_begin();
    for (int i = 0; i <= n; ++i) {
        const int l = ((start.line - i) % n + n) % n;
        if (!wrap && start.line - i < 0) break;
        const std::string& s = m.lines[l];
        size_t best = std::string_view::npos;
        for (size_t at = find_in(s, needle, 0, case_sensitive); at != std::string_view::npos;
             at = find_in(s, needle, at + 1, case_sensitive)) {
            if (i == 0 && static_cast<int>(at) >= start.col) break;
            if (i == n && static_cast<int>(at) < start.col) continue;
            best = at;
        }
        if (best != std::string_view::npos) return select(l, best);
    }
    return false;
}

std::vector<TextPos> TextBuffer::find_all(std::string_view needle, bool case_sensitive) const {
    std::vector<TextPos> out;
    if (needle.empty() || needle.find('\n') != std::string_view::npos) return out;
    for (int l = 0; l < impl_->count(); ++l) {
        const std::string& s = impl_->lines[l];
        for (size_t at = find_in(s, needle, 0, case_sensitive); at != std::string_view::npos;
             at = find_in(s, needle, at + needle.size(), case_sensitive))
            out.push_back({l, static_cast<int>(at)});
    }
    return out;
}

// ---- saved state / settings -----------------------------------------------------------

void TextBuffer::mark_saved() {
    impl_->saved_id = impl_->current_id();
    impl_->merge_blocked = true;
}

bool TextBuffer::modified() const { return impl_->current_id() != impl_->saved_id; }
int TextBuffer::tab_size() const { return impl_->tab_size; }
void TextBuffer::set_tab_size(int spaces) { impl_->tab_size = std::clamp(spaces, 1, 16); }

// ---- view helpers -------------------------------------------------------------------

int TextBuffer::visual_column(int line, int col) const {
    return impl_->visual_column(std::clamp(line, 0, impl_->count() - 1), col);
}

int TextBuffer::column_at_visual(int line, float vcol) const {
    return impl_->column_at_visual(std::clamp(line, 0, impl_->count() - 1), vcol);
}

bool TextBuffer::matching_bracket(TextPos pos, TextPos* bracket, TextPos* match) const {
    const Impl& m = *impl_;
    const TextPos p = m.clamp(pos);
    static constexpr std::string_view kOpen = "([{", kClose = ")]}";
    auto kind_at = [&](TextPos q) -> char {
        if (q.col < 0 || q.col >= m.len(q.line)) return 0;
        const char c = m.lines[q.line][q.col];
        return (kOpen.find(c) != std::string_view::npos || kClose.find(c) != std::string_view::npos) ? c : 0;
    };
    TextPos at = p;
    char c = kind_at(at);
    if (!c) {
        at = {p.line, p.col - 1};
        c = kind_at(at);
    }
    if (!c) return false;

    const size_t oi = kOpen.find(c);
    const bool forward = oi != std::string_view::npos;
    const char open = forward ? c : kOpen[kClose.find(c)];
    const char close = forward ? kClose[oi] : c;
    int depth = 0;
    long budget = 1 << 20;  // characters scanned; keeps huge files responsive
    TextPos q = at;
    while (budget-- > 0) {
        const char ch = m.lines[q.line][q.col];
        if (ch == open) depth += forward ? 1 : -1;
        if (ch == close) depth += forward ? -1 : 1;
        if (depth == 0) {
            if (bracket) *bracket = at;
            if (match) *match = q;
            return true;
        }
        if (forward) {
            ++q.col;
            while (q.col >= m.len(q.line)) {
                if (q.line + 1 >= m.count()) return false;
                q = {q.line + 1, 0};
            }
        } else {
            --q.col;
            while (q.col < 0) {
                if (q.line == 0) return false;
                --q.line;
                q.col = m.len(q.line) - 1;
            }
        }
    }
    return false;
}

} // namespace osef::editor
