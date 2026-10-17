#include "model/hardware_decl.h"

#include "model/cpp_tokens.h"

#include <cstdio>

namespace osef::editor {

namespace {

using cpp::Token;
using cpp::TokKind;

constexpr size_t kNameMax = 31;  // osef::kHardwareNameMax - 1

// Same wording as the static_asserts / diagnostics in osef/hardware.h.
constexpr const char* kMissingArch = "OSEF_HARDWARE: choose a CPU architecture: .arch(osef::Arch::x86_64)";
constexpr const char* kUnsupportedArch = "OSEF_HARDWARE: unsupported architecture (supported: x86_64)";
constexpr const char* kMissingFirmware =
    "OSEF_HARDWARE: choose the boot firmware: .firmware(osef::Firmware::BIOS) or .firmware(osef::Firmware::UEFI)";
constexpr const char* kBadFirmware = "OSEF_HARDWARE: firmware must be osef::Firmware::BIOS or ::UEFI";
constexpr const char* kMissingConsole =
    "OSEF_HARDWARE: enable at least one console device: .devices(osef::Hw::Serial) and/or osef::Hw::Framebuffer";
constexpr const char* kLowMemory = "OSEF_HARDWARE: .memory_mb() must be at least 32";
constexpr const char* kHalfResolution = "OSEF_HARDWARE: .resolution() needs both width and height";
constexpr const char* kBadBaud = "OSEF_HARDWARE: .serial_baud() must divide 115200";
constexpr const char* kEmptyName = "OSEF_HARDWARE: .name() must not be empty";
constexpr const char* kLongName = "OSEF_HARDWARE: .name() is too long (max 31 characters)";

constexpr uint32_t kSerial = 1u << 0, kFramebuffer = 1u << 1, kPs2Keyboard = 1u << 2;
constexpr uint32_t kRtc = 1u << 3, kPci = 1u << 4, kUsb = 1u << 5;
constexpr uint32_t kAllDevices = kSerial | kFramebuffer | kPs2Keyboard | kRtc | kPci | kUsb;

std::string_view newline_of(std::string_view source) {
    size_t lf = source.find('\n');
    return (lf != std::string_view::npos && lf > 0 && source[lf - 1] == '\r') ? "\r\n" : "\n";
}

// A token range [a, b) inside the declaration.
struct Span {
    size_t a = 0;
    size_t b = 0;
    bool empty() const { return a >= b; }
    size_t size() const { return b > a ? b - a : 0; }
};

class DeclParser {
public:
    DeclParser(std::string_view src, const std::vector<Token>& toks, HardwareParse& out)
        : src_(src), t_(toks), out_(out) {}

    bool lossy = false;  // something was not understood (would be dropped by a rewrite)

    void parse(Span whole) {
        // Redundant outer parentheses: OSEF_HARDWARE((osef::Hardware()...)).
        while (whole.size() >= 2 && t_[whole.a].is("(") && cpp::match_bracket(t_, whole.a) == whole.b - 1) {
            ++whole.a;
            --whole.b;
        }
        if (whole.empty()) {
            problem(whole.a, "the declaration is empty: start it with osef::Hardware()");
            lossy = true;
            return;
        }
        size_t pos = whole.a;
        if (!parse_base(pos, whole.b)) {
            problem(whole.a, "the declaration must start with osef::Hardware()");
            lossy = true;
            pos = whole.a;  // skip ahead to the first top-level builder call
            int depth = 0;
            while (pos < whole.b) {
                const Token& tk = t_[pos];
                if (tk.is("(") || tk.is("[") || tk.is("{")) ++depth;
                if (tk.is(")") || tk.is("]") || tk.is("}")) --depth;
                if (depth == 0 && tk.is(".")) break;
                ++pos;
            }
        }
        while (pos < whole.b) {
            if (!t_[pos].is(".") || pos + 2 >= whole.b || t_[pos + 1].kind != TokKind::Identifier ||
                !t_[pos + 2].is("(")) {
                problem(pos, "unexpected '" + std::string(t_[pos].text) +
                                 "' (expected a builder call such as .memory_mb(128))");
                lossy = true;
                return;
            }
            size_t open = pos + 2;
            size_t close = cpp::match_bracket(t_, open);
            if (close == cpp::npos || close >= whole.b) {
                problem(pos, "unbalanced parentheses in ." + std::string(t_[pos + 1].text) + "(...)");
                lossy = true;
                return;
            }
            call(pos + 1, Span{open + 1, close});
            pos = close + 1;
        }
    }

    void finish() {
        HardwareDecl& d = out_.decl;
        if (d.name.empty()) out_.problems.push_back(kEmptyName);
        else if (d.name.size() > kNameMax) out_.problems.push_back(kLongName);
        if (d.arch.empty()) out_.problems.push_back(kMissingArch);
        else if (d.arch != "x86_64") out_.problems.push_back(kUnsupportedArch);
        if (d.firmware.empty()) out_.problems.push_back(kMissingFirmware);
        else if (d.firmware != "BIOS" && d.firmware != "UEFI") out_.problems.push_back(kBadFirmware);
        if (!d.serial && !d.framebuffer) out_.problems.push_back(kMissingConsole);
        if (d.memory_mb < 32) out_.problems.push_back(kLowMemory);
        if ((d.fb_width == 0) != (d.fb_height == 0)) out_.problems.push_back(kHalfResolution);
        if (d.serial_baud == 0 || 115200 % d.serial_baud != 0) out_.problems.push_back(kBadBaud);
    }

private:
    void problem(size_t tok, const std::string& what) {
        size_t off = tok < t_.size() ? t_[tok].begin : src_.size();
        out_.problems.push_back("line " + std::to_string(cpp::line_of(src_, off)) + ": " + what);
    }

    // [::] [osef ::] Hardware ( )
    bool parse_base(size_t& pos, size_t end) {
        size_t p = pos;
        if (p < end && t_[p].is("::")) ++p;
        if (p + 1 < end && t_[p].is("osef") && t_[p + 1].is("::")) p += 2;
        if (p + 2 < end && t_[p].is("Hardware") && t_[p + 1].is("(") && t_[p + 2].is(")")) {
            pos = p + 3;
            return true;
        }
        return false;
    }

    // [::] [osef ::] <scope> :: <Ident>  ->  Ident
    bool parse_scoped(Span s, std::string_view scope, std::string& value) const {
        size_t p = s.a;
        if (p < s.b && t_[p].is("::")) ++p;
        if (p + 1 < s.b && t_[p].is("osef") && t_[p + 1].is("::")) p += 2;
        if (p + 3 == s.b && t_[p].is(scope) && t_[p + 1].is("::") && t_[p + 2].kind == TokKind::Identifier) {
            value = std::string(t_[p + 2].text);
            return true;
        }
        return false;
    }

    bool parse_uint(Span s, uint32_t& value) const {
        // Allow redundant parentheses and a unary '+'.
        while (s.size() >= 2 && t_[s.a].is("(") && cpp::match_bracket(t_, s.a) == s.b - 1) {
            ++s.a;
            --s.b;
        }
        if (s.size() == 2 && t_[s.a].is("+")) ++s.a;
        if (s.size() != 1 || t_[s.a].kind != TokKind::Number) return false;
        std::string digits;
        for (char c : t_[s.a].text)
            if (c != '\'') digits += c;
        // Integer suffix: u/U, optionally with l/L/ll/LL (any order C++ allows).
        while (!digits.empty()) {
            char c = digits.back();
            if (c == 'u' || c == 'U' || c == 'l' || c == 'L') digits.pop_back();
            else break;
        }
        if (digits.empty()) return false;
        int base = 10;
        size_t i = 0;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            i = 2;
        } else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
            base = 2;
            i = 2;
        } else if (digits.size() > 1 && digits[0] == '0') {
            base = 8;
            i = 1;
        }
        uint64_t v = 0;
        for (; i < digits.size(); ++i) {
            char c = digits[i];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                                     : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99;
            if (d >= base) return false;
            v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
            if (v > 0xFFFFFFFFull) return false;
        }
        value = static_cast<uint32_t>(v);
        return true;
    }

    // devices := term ('|' term)* ; term := '(' devices ')' | Hw::X | integer
    bool parse_devices(Span s, uint32_t& flags, size_t call_tok) {
        flags = 0;
        size_t p = s.a;
        if (s.empty()) return false;
        for (;;) {
            // Find the end of this term: the next top-level '|'.
            size_t q = p;
            int depth = 0;
            while (q < s.b) {
                if (t_[q].is("(")) ++depth;
                else if (t_[q].is(")")) --depth;
                else if (depth == 0 && t_[q].is("|")) break;
                ++q;
            }
            Span term{p, q};
            if (term.empty()) return false;
            uint32_t f = 0;
            if (!parse_device_term(term, f, call_tok)) return false;
            flags |= f;
            if (q >= s.b) return true;
            p = q + 1;
        }
    }

    bool parse_device_term(Span term, uint32_t& f, size_t call_tok) {
        if (term.size() >= 2 && t_[term.a].is("(") && cpp::match_bracket(t_, term.a) == term.b - 1)
            return parse_devices(Span{term.a + 1, term.b - 1}, f, call_tok);
        std::string flag;
        if (parse_scoped(term, "Hw", flag)) {
            if (flag == "Serial") f = kSerial;
            else if (flag == "Framebuffer") f = kFramebuffer;
            else if (flag == "PS2Keyboard") f = kPs2Keyboard;
            else if (flag == "RTC") f = kRtc;
            else if (flag == "PCI") f = kPci;
            else if (flag == "USB") f = kUsb;
            else if (flag == "kAll") f = kAllDevices;
            else {
                problem(term.a, "unknown device flag osef::Hw::" + flag +
                                    " (known: Serial, Framebuffer, PS2Keyboard, RTC, PCI, USB, kAll)");
                lossy = true;
                f = 0;
            }
            return true;
        }
        uint32_t v = 0;
        if (parse_uint(term, v)) {
            if (v & ~kAllDevices) {
                problem(call_tok, "unknown device flag bits in .devices(): 0x" + hex(v & ~kAllDevices));
                lossy = true;
            }
            f = v & kAllDevices;
            return true;
        }
        return false;
    }

    static std::string hex(uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%X", v);
        return buf;
    }

    bool parse_string(Span s, std::string& value) const {
        if (s.empty()) return false;
        value.clear();
        for (size_t p = s.a; p < s.b; ++p) {
            const Token& tk = t_[p];
            if (tk.kind != TokKind::String || !tk.ok) return false;
            // Only narrow literals convert to const char*: "..." and R"(...)".
            if (!(tk.text.front() == '"' || tk.text.front() == 'R')) return false;
            value += tk.value;  // adjacent literals concatenate
        }
        return true;
    }

    std::vector<Span> split_args(Span s) const {
        std::vector<Span> args;
        if (s.empty()) return args;
        size_t start = s.a;
        int depth = 0;
        for (size_t p = s.a; p < s.b; ++p) {
            const Token& tk = t_[p];
            if (tk.is("(") || tk.is("[") || tk.is("{")) ++depth;
            else if (tk.is(")") || tk.is("]") || tk.is("}")) --depth;
            else if (depth == 0 && tk.is(",")) {
                args.push_back(Span{start, p});
                start = p + 1;
            }
        }
        args.push_back(Span{start, s.b});
        return args;
    }

    void bad_args(size_t name_tok, const std::string& expected) {
        problem(name_tok, "could not read the argument of ." + std::string(t_[name_tok].text) + "(...): expected " +
                              expected);
        lossy = true;
    }

    void call(size_t name_tok, Span inner) {
        const std::string_view m = t_[name_tok].text;
        HardwareDecl& d = out_.decl;
        std::vector<Span> args = split_args(inner);
        auto one_uint = [&](uint32_t& field, const char* expected) {
            uint32_t v = 0;
            if (args.size() == 1 && parse_uint(args[0], v)) field = v;
            else bad_args(name_tok, expected);
        };
        if (m == "name") {
            std::string v;
            if (args.size() == 1 && parse_string(args[0], v)) d.name = v;
            else bad_args(name_tok, "a string literal such as \"MyOS\"");
        } else if (m == "arch") {
            std::string v;
            if (args.size() == 1 && parse_scoped(args[0], "Arch", v)) d.arch = v == "Unspecified" ? "" : v;
            else bad_args(name_tok, "osef::Arch::x86_64");
        } else if (m == "firmware") {
            std::string v;
            if (args.size() == 1 && parse_scoped(args[0], "Firmware", v)) d.firmware = v == "Unspecified" ? "" : v;
            else bad_args(name_tok, "osef::Firmware::BIOS or osef::Firmware::UEFI");
        } else if (m == "devices") {
            uint32_t f = 0;
            if (args.size() == 1 && parse_devices(args[0], f, name_tok)) {
                d.serial = (f & kSerial) != 0;
                d.framebuffer = (f & kFramebuffer) != 0;
                d.ps2_keyboard = (f & kPs2Keyboard) != 0;
                d.rtc = (f & kRtc) != 0;
                d.pci = (f & kPci) != 0;
                d.usb = (f & kUsb) != 0;
            } else {
                bad_args(name_tok, "osef::Hw flags joined with |, e.g. osef::Hw::Serial | osef::Hw::Framebuffer");
            }
        } else if (m == "memory_mb") {
            one_uint(d.memory_mb, "an integer number of MiB");
        } else if (m == "serial_baud") {
            one_uint(d.serial_baud, "an integer baud rate");
        } else if (m == "boot_timeout") {
            one_uint(d.boot_timeout_s, "an integer number of seconds");
        } else if (m == "resolution") {
            uint32_t w = 0, h = 0;
            if (args.size() == 2 && parse_uint(args[0], w) && parse_uint(args[1], h)) {
                d.fb_width = w;
                d.fb_height = h;
            } else {
                bad_args(name_tok, "width and height, e.g. .resolution(1280, 720)");
            }
        } else {
            problem(name_tok, "unknown builder call ." + std::string(m) +
                                  "(...) (known: name, arch, firmware, devices, memory_mb, serial_baud, "
                                  "resolution, boot_timeout)");
            lossy = true;
        }
    }

    std::string_view src_;
    const std::vector<Token>& t_;
    HardwareParse& out_;
};

HardwareParse parse_impl(std::string_view source, bool* lossy) {
    HardwareParse out;
    if (lossy) *lossy = false;
    cpp::Tokens toks = cpp::tokenize(source);
    const std::vector<Token>& t = toks.tokens;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i].kind != TokKind::Identifier || t[i].text != "OSEF_HARDWARE" || !t[i + 1].is("(")) continue;
        size_t close = cpp::match_bracket(t, i + 1);
        if (close == cpp::npos || close + 1 >= t.size() || !t[close + 1].is(";")) continue;
        if (out.found) {
            out.problems.push_back("line " + std::to_string(cpp::line_of(source, t[i].begin)) +
                                   ": another OSEF_HARDWARE declaration (an OS declares its hardware exactly once)");
            i = close + 1;
            continue;
        }
        out.found = true;
        out.begin = t[i].begin;
        out.end = t[close + 1].end;
        out.line = cpp::line_of(source, out.begin);
        DeclParser p(source, t, out);
        p.parse(Span{i + 2, close});
        p.finish();
        if (lossy) *lossy = p.lossy;
        i = close + 1;
    }
    return out;
}

void append_escaped(std::string& out, std::string_view s) {
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20 || c == 0x7F) {
                // Three-digit octal: never swallows a following digit (unlike \x).
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03o", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
}

std::string generate_impl(const HardwareDecl& d, std::string_view nl) {
    std::string out = "OSEF_HARDWARE(osef::Hardware()";
    auto line = [&](const std::string& call) {
        out += nl;
        out += "    ";
        out += call;
    };
    std::string name = ".name(\"";
    append_escaped(name, d.name);
    name += "\")";
    line(name);
    if (!d.arch.empty()) line(".arch(osef::Arch::" + d.arch + ")");
    if (!d.firmware.empty()) line(".firmware(osef::Firmware::" + d.firmware + ")");
    std::string devices;
    auto add_device = [&](bool on, const char* flag) {
        if (!on) return;
        if (!devices.empty()) devices += " | ";
        devices += flag;
    };
    add_device(d.serial, "osef::Hw::Serial");
    add_device(d.framebuffer, "osef::Hw::Framebuffer");
    add_device(d.ps2_keyboard, "osef::Hw::PS2Keyboard");
    add_device(d.rtc, "osef::Hw::RTC");
    add_device(d.pci, "osef::Hw::PCI");
    add_device(d.usb, "osef::Hw::USB");
    if (!devices.empty()) line(".devices(" + devices + ")");
    line(".memory_mb(" + std::to_string(d.memory_mb) + ")");
    if (d.serial_baud != 115200) line(".serial_baud(" + std::to_string(d.serial_baud) + ")");
    if (d.fb_width != 0 || d.fb_height != 0)
        line(".resolution(" + std::to_string(d.fb_width) + ", " + std::to_string(d.fb_height) + ")");
    if (d.boot_timeout_s != 0) line(".boot_timeout(" + std::to_string(d.boot_timeout_s) + ")");
    out += ");";
    return out;
}

// Does `s` start with a whitespace-only line (or is it empty)?
bool starts_with_blank_line(std::string_view s) {
    for (char c : s) {
        if (c == '\n') return true;
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return false;  // only whitespace up to EOF without a newline: not a blank line
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Inserts `block` (lines joined with nl, no trailing newline) at byte offset
// `at`, separated from surrounding text by one blank line on each side.
std::string insert_block(std::string_view source, size_t at, const std::string& block, std::string_view nl) {
    std::string_view prefix = source.substr(0, at);
    std::string_view rest = source.substr(at);
    std::string out(prefix);
    if (!prefix.empty()) {
        if (!ends_with(prefix, "\n")) {
            out += nl;
            out += nl;
        } else {
            // Ends with a newline: add a blank line unless the previous line already is one.
            std::string_view before = prefix.substr(0, prefix.size() - 1);
            if (ends_with(before, "\r")) before.remove_suffix(1);
            size_t prev = before.find_last_of('\n');
            std::string_view last_line = prev == std::string_view::npos ? before : before.substr(prev + 1);
            bool blank = last_line.find_first_not_of(" \t\r") == std::string_view::npos;
            if (!blank) out += nl;
        }
    }
    out += block;
    out += nl;
    if (!rest.empty() && !starts_with_blank_line(rest)) out += nl;
    out += rest;
    return out;
}

// Offset just past the line containing `offset` (past its newline), or EOF.
size_t end_of_line(std::string_view s, size_t offset) {
    size_t lf = s.find('\n', offset);
    return lf == std::string_view::npos ? s.size() : lf + 1;
}

// Offset of the `#include <osef/hardware.h>` line's end, or npos.
size_t find_hardware_include(std::string_view source, const cpp::Tokens& toks) {
    const std::vector<Token>& t = toks.tokens;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (!t[i].is("#") || !t[i + 1].is("include")) continue;
        // '#' must start its line.
        size_t ls = source.rfind('\n', t[i].begin == 0 ? 0 : t[i].begin - 1);
        ls = (ls == std::string_view::npos || t[i].begin == 0) ? 0 : ls + 1;
        if (source.substr(ls, t[i].begin - ls).find_first_not_of(" \t") != std::string_view::npos) continue;
        size_t eol = end_of_line(source, t[i + 1].end);
        std::string_view rest = source.substr(t[i + 1].end, eol - t[i + 1].end);
        size_t b = rest.find_first_not_of(" \t");
        if (b == std::string_view::npos) continue;
        rest = rest.substr(b);
        if (rest.starts_with("<osef/hardware.h>") || rest.starts_with("\"osef/hardware.h\"")) return eol;
    }
    return std::string_view::npos;
}

// Offset where text goes after the file's leading comment block: the end of
// the line holding the block's last comment. 0 when the file has none.
size_t after_leading_comments(std::string_view source, const cpp::Tokens& toks) {
    size_t pos = source.find_first_not_of(" \t\r\n");
    if (pos == std::string_view::npos) return 0;
    size_t block_end = 0;
    bool any = false;
    size_t ci = 0;
    for (;;) {
        while (ci < toks.comments.size() && toks.comments[ci].begin < pos) ++ci;
        if (ci >= toks.comments.size() || toks.comments[ci].begin != pos) break;
        block_end = toks.comments[ci].end;
        any = true;
        // The next comment continues the block only if it starts on the next
        // line (or the same line): a blank line ends the block.
        size_t p = block_end;
        while (p < source.size() && (source[p] == ' ' || source[p] == '\t')) ++p;
        if (p < source.size() && source[p] == '\r') ++p;
        if (p < source.size() && source[p] == '\n') {
            ++p;
            while (p < source.size() && (source[p] == ' ' || source[p] == '\t')) ++p;
        }
        pos = p;
    }
    if (!any) return 0;
    // Only whitespace after the last comment on its line: insert after the line.
    size_t eol = end_of_line(source, block_end);
    std::string_view tail = source.substr(block_end, eol - block_end);
    if (tail.find_first_not_of(" \t\r\n") == std::string_view::npos) return eol;
    return block_end;
}

} // namespace

HardwareParse parse_hardware_decl(std::string_view source) { return parse_impl(source, nullptr); }

bool hardware_decl_has_unparsed_code(std::string_view source) {
    bool lossy = false;
    parse_impl(source, &lossy);
    return lossy;
}

std::string generate_hardware_decl(const HardwareDecl& decl) { return generate_impl(decl, "\n"); }

std::string apply_hardware_decl(std::string_view source, const HardwareDecl& decl) {
    const std::string_view nl = newline_of(source);
    const std::string text = generate_impl(decl, nl);
    HardwareParse parsed = parse_hardware_decl(source);
    if (parsed.found) {
        std::string out(source.substr(0, parsed.begin));
        out += text;
        out += source.substr(parsed.end);
        return out;
    }
    cpp::Tokens toks = cpp::tokenize(source);
    size_t at = find_hardware_include(source, toks);
    if (at != std::string_view::npos) return insert_block(source, at, text, nl);
    std::string block = "#include <osef/hardware.h>";
    block += nl;
    block += nl;
    block += text;
    return insert_block(source, after_leading_comments(source, toks), block, nl);
}

} // namespace osef::editor
