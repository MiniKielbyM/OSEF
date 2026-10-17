// osef-image: turns an OSEF kernel ELF into a bootable ISO.
//
// The kernel carries its OSEF_HARDWARE declaration in the ELF section
// ".osef_hardware". This tool reads it and builds an ISO whose boot path
// matches the declared firmware exactly (BIOS-only or UEFI-only), using the
// Limine bootloader and xorriso.
//
//   osef-image check-source [--no-declaration] <file.cpp>
//   osef-image inspect <kernel.elf>
//   osef-image iso <kernel.elf> -o <out.iso> --limine <dir> [--xorriso <exe>] [--limine-tool <exe>]
//   osef-image run <out.iso> --kernel <kernel.elf> [--qemu <exe>] [--ovmf <code.fd>]
//                  [--headless] [--test] [--timeout <s>] [-- <extra qemu args>]
//
// check-source: main.cpp must declare OSEF_HARDWARE(...) (with a .name() of
// at most 31 characters); every OS source gets a warning for C/C++ standard
// library use outside `#if OSEF_HOSTED`, which the freestanding ISO kernel
// cannot compile. --no-declaration: only the warnings (non-main sources).
//
// run --test exit status: the OS's exit code (0..125; codes outside that
// range are reported as 125), 124 timeout, 126 QEMU failed or the VM stopped
// without an OSEF exit code, 127 kernel panic.

#include <osef/hardware.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <ctime>
#endif

namespace fs = std::filesystem;

const char* osef::to_string(osef::Arch arch) {
    return arch == osef::Arch::x86_64 ? "x86_64" : "unspecified";
}

const char* osef::to_string(osef::Firmware fw) {
    switch (fw) {
        case osef::Firmware::BIOS: return "BIOS";
        case osef::Firmware::UEFI: return "UEFI";
        default: return "unspecified";
    }
}

namespace {

// Exit status for errors; `run --test` uses kRunToolError so that a tool
// error can never be mistaken for an OS exit code of 1.
int g_error_status = 1;

[[noreturn]] void die(const std::string& msg) {
    std::cerr << "osef-image: error: " << msg << "\n";
    std::exit(g_error_status);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) die("cannot read " + p.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out || !(out << data)) die("cannot write " + p.string());
}

// ---------------------------------------------------------------- ELF

template <class T>
T load(const std::string& buf, uint64_t off) {
    if (off > buf.size() || buf.size() - off < sizeof(T)) die("truncated ELF file");
    T v;
    std::memcpy(&v, buf.data() + off, sizeof(T));
    return v;
}

struct Elf64Header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Elf64Section {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};

std::optional<std::string> elf_section(const std::string& elf, const std::string& wanted) {
    if (elf.size() < sizeof(Elf64Header) || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
        die("not an ELF file");
    auto eh = load<Elf64Header>(elf, 0);
    if (eh.ident[4] != 2 || eh.ident[5] != 1) die("kernel must be a 64-bit little-endian ELF");
    if (eh.machine != 62) die("kernel ELF is not x86_64 (e_machine=" + std::to_string(eh.machine) + ")");
    if (eh.shoff == 0 || eh.shnum == 0) die("kernel ELF has no section headers");
    if (eh.shentsize != sizeof(Elf64Section)) die("unexpected ELF section header size");
    if (eh.shstrndx >= eh.shnum) die("bad ELF string table index");
    auto strtab = load<Elf64Section>(elf, eh.shoff + uint64_t(eh.shstrndx) * eh.shentsize);
    for (uint16_t i = 0; i < eh.shnum; ++i) {
        auto sh = load<Elf64Section>(elf, eh.shoff + uint64_t(i) * eh.shentsize);
        uint64_t name_off = strtab.offset + sh.name;
        if (name_off >= elf.size()) die("bad ELF section name offset");
        std::string name(elf.c_str() + name_off, strnlen(elf.c_str() + name_off, elf.size() - name_off));
        if (name != wanted) continue;
        if (sh.type == 8 /* SHT_NOBITS */) die("section " + wanted + " has no file contents");
        if (sh.offset > elf.size() || elf.size() - sh.offset < sh.size) die("section " + wanted + " is truncated");
        return elf.substr(sh.offset, sh.size);
    }
    return std::nullopt;
}

osef::HardwareSpec read_spec(const fs::path& kernel) {
    std::string elf = read_file(kernel);
    auto sec = elf_section(elf, ".osef_hardware");
    if (!sec)
        die(kernel.string() + " has no .osef_hardware section.\n"
            "  Declare the target hardware at the top of your main .cpp:\n"
            "    OSEF_HARDWARE(osef::Hardware().arch(osef::Arch::x86_64)\n"
            "        .firmware(osef::Firmware::UEFI).devices(osef::Hw::Serial));");
    if (sec->size() < sizeof(osef::HardwareSpec)) die(".osef_hardware section is too small");
    osef::HardwareSpec spec;
    std::memcpy(&spec, sec->data(), sizeof(spec));
    if (spec.magic != osef::kHardwareMagic) die(".osef_hardware has a bad magic number");
    if (spec.version != osef::kHardwareVersion)
        die(".osef_hardware version " + std::to_string(spec.version) + " is not supported by this tool (expected " +
            std::to_string(osef::kHardwareVersion) + "); rebuild the tool and the kernel from the same OSEF");
    if (spec.size != sizeof(osef::HardwareSpec)) die(".osef_hardware has an unexpected size");
    if (spec.arch != uint32_t(osef::Arch::x86_64)) die("unsupported architecture in hardware declaration");
    if (spec.firmware != uint32_t(osef::Firmware::BIOS) && spec.firmware != uint32_t(osef::Firmware::UEFI))
        die("hardware declaration must choose firmware BIOS or UEFI");
    if (!(spec.devices & (osef::Hw::Serial | osef::Hw::Framebuffer))) die("hardware declaration has no console device");
    spec.name[osef::kHardwareNameMax - 1] = '\0';
    if (spec.name[0] == '\0') die("hardware declaration has an empty name");
    return spec;
}

osef::Firmware firmware_of(const osef::HardwareSpec& s) { return osef::Firmware(s.firmware); }

std::string devices_string(uint32_t d) {
    std::string out;
    auto add = [&](uint32_t bit, const char* name) {
        if (!(d & bit)) return;
        if (!out.empty()) out += ",";
        out += name;
    };
    add(osef::Hw::Serial, "serial");
    add(osef::Hw::Framebuffer, "framebuffer");
    add(osef::Hw::PS2Keyboard, "ps2-keyboard");
    add(osef::Hw::RTC, "rtc");
    add(osef::Hw::PCI, "pci");
    return out.empty() ? "none" : out;
}

// File-system friendly version of the OS name: "My OS!" -> "my-os".
std::string slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c >= 'A' && c <= 'Z') out += char(c - 'A' + 'a');
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "kernel" : out;
}

// ---------------------------------------------------------------- processes

std::string quote(const std::string& arg) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}

std::string command_line(const std::vector<std::string>& args) {
    std::string cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? "" : " ") + quote(a);
    return cmd;
}

// Runs a program, returns its exit status (or -1 if it could not run / timed out).
int run(const std::vector<std::string>& args, int timeout_s = 0, bool verbose = true) {
    if (verbose) std::cerr << "+ " << command_line(args) << "\n";
#if defined(_WIN32)
    (void)timeout_s;
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], argv.data());
    return rc < 0 ? -1 : int(rc);
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "osef-image: cannot run %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    time_t deadline = timeout_s > 0 ? std::time(nullptr) + timeout_s : 0;
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, timeout_s > 0 ? WNOHANG : 0);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (timeout_s > 0 && std::time(nullptr) >= deadline) {
            std::cerr << "osef-image: timed out after " << timeout_s << "s, killing " << args[0] << "\n";
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        if (timeout_s > 0) usleep(50 * 1000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool on_path(const std::string& exe) {
    if (exe.find('/') != std::string::npos || exe.find('\\') != std::string::npos) return fs::exists(exe);
    const char* path = std::getenv("PATH");
    if (!path) return false;
#if defined(_WIN32)
    const char sep = ';';
    const char* exts[] = {"", ".exe"};
#else
    const char sep = ':';
    const char* exts[] = {""};
#endif
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, sep))
        for (const char* ext : exts)
            if (!dir.empty() && fs::exists(fs::path(dir) / (exe + ext))) return true;
    return false;
}

// ---------------------------------------------------------------- commands

struct Args {
    std::vector<std::string> positional;
    std::vector<std::string> passthrough;
    std::string out, limine, xorriso = "xorriso", limine_tool, qemu = "qemu-system-x86_64", ovmf, kernel;
    bool headless = false, test = false, no_declaration = false;
    int timeout = 0;
};

Args parse(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; ++i) {
        std::string s = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value for " + s);
            return argv[++i];
        };
        if (s == "--") {
            for (++i; i < argc; ++i) a.passthrough.push_back(argv[i]);
        } else if (s == "-o" || s == "--output") a.out = value();
        else if (s == "--limine") a.limine = value();
        else if (s == "--xorriso") a.xorriso = value();
        else if (s == "--limine-tool") a.limine_tool = value();
        else if (s == "--qemu") a.qemu = value();
        else if (s == "--ovmf") a.ovmf = value();
        else if (s == "--kernel") a.kernel = value();
        else if (s == "--headless") a.headless = true;
        else if (s == "--no-declaration") a.no_declaration = true;
        else if (s == "--test") a.test = a.headless = true;
        else if (s == "--timeout") a.timeout = std::atoi(value().c_str());
        else if (!s.empty() && s[0] == '-') die("unknown option " + s);
        else a.positional.push_back(s);
    }
    return a;
}

// ---------------------------------------------------------------- source checks

// A C++ source split into two views of the same length (offsets and line
// numbers are identical in both):
//   code:     comments and the contents of string/char literals blanked out,
//             so declarations inside them never count;
//   literals: only comments blanked out (literals kept, for reading .name("...")).
// Handles // and /* */ comments, "..." and '...' literals with escapes, raw
// strings R"delim(...)delim" (any encoding prefix), and C++14 digit
// separators (64'000 is a number, not the start of a character literal).
struct SourceViews {
    std::string code, literals;
};

bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Start of the identifier / pp-number token that ends just before `end`.
size_t token_start(const std::string& s, size_t end) {
    size_t b = end;
    while (b > 0 && (ident_char(s[b - 1]) || s[b - 1] == '\'' || s[b - 1] == '.')) --b;
    return b;
}

SourceViews split_source(const std::string& src) {
    SourceViews v{src, src};
    auto blank = [&](std::string& s, size_t from, size_t to) {
        for (size_t k = from; k < to && k < s.size(); ++k)
            if (s[k] != '\n') s[k] = ' ';
    };
    size_t i = 0;
    while (i < src.size()) {
        const char c = src[i];
        const char n = i + 1 < src.size() ? src[i + 1] : '\0';
        if (c == '/' && n == '/') {
            size_t e = src.find('\n', i);
            if (e == std::string::npos) e = src.size();
            blank(v.code, i, e);
            blank(v.literals, i, e);
            i = e;
        } else if (c == '/' && n == '*') {
            size_t e = src.find("*/", i + 2);
            e = e == std::string::npos ? src.size() : e + 2;
            blank(v.code, i, e);
            blank(v.literals, i, e);
            i = e;
        } else if (c == '\'' && i > 0 && token_start(src, i) < i && src[token_start(src, i)] >= '0' &&
                   src[token_start(src, i)] <= '9') {
            ++i;  // digit separator inside a number: 64'000, 0xFF'FF
        } else if (c == '"' || c == '\'') {
            // Raw string? The token right before the quote is R, u8R, uR, UR or LR.
            const size_t b = token_start(src, i);
            const std::string prefix = src.substr(b, i - b);
            size_t e;
            if (c == '"' && (prefix == "R" || prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR")) {
                const size_t open = src.find('(', i + 1);
                const std::string delim = open == std::string::npos ? std::string() : src.substr(i + 1, open - i - 1);
                const size_t close = open == std::string::npos ? std::string::npos : src.find(")" + delim + "\"", open + 1);
                e = close == std::string::npos ? src.size() : close + delim.size() + 2;
            } else {
                e = i + 1;
                while (e < src.size() && src[e] != c && src[e] != '\n') e += src[e] == '\\' ? 2 : 1;
                e = e < src.size() ? e + 1 : src.size();
            }
            blank(v.code, i, e);
            i = e;
        } else {
            ++i;
        }
    }
    return v;
}

size_t line_at(const std::string& s, size_t pos) {
    return 1 + size_t(std::count(s.begin(), s.begin() + long(std::min(pos, s.size())), '\n'));
}

size_t skip_space(const std::string& s, size_t j) {
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) ++j;
    return j;
}

// Reads one or more adjacent ordinary string literals starting at `j` in the
// literals view; returns false if there is no plain literal there.
bool read_string_literal(const std::string& s, size_t j, std::string& out) {
    bool any = false;
    out.clear();
    for (j = skip_space(s, j); j < s.size(); j = skip_space(s, j)) {
        if (s.compare(j, 2, "u8") == 0 && j + 2 < s.size() && s[j + 2] == '"') j += 2;
        if (s[j] != '"') break;
        any = true;
        for (++j; j < s.size() && s[j] != '"' && s[j] != '\n'; ++j) {
            if (s[j] != '\\' || j + 1 >= s.size()) {
                out += s[j];
                continue;
            }
            const char e = s[++j];
            if (e == 'x') {  // \xHH...
                unsigned v = 0;
                while (j + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[j + 1]))) {
                    const char h = char(std::tolower(static_cast<unsigned char>(s[++j])));
                    v = v * 16 + unsigned(h <= '9' ? h - '0' : h - 'a' + 10);
                }
                out += char(v);
            } else if (e >= '0' && e <= '7') {  // \ooo
                unsigned v = unsigned(e - '0');
                for (int k = 0; k < 2 && j + 1 < s.size() && s[j + 1] >= '0' && s[j + 1] <= '7'; ++k)
                    v = v * 8 + unsigned(s[++j] - '0');
                out += char(v);
            } else {
                const char* map = "n\nt\tr\ra\ab\bf\fv\v";
                char r = e;
                for (const char* m = map; *m; m += 2)
                    if (*m == e) r = m[1];
                out += r;
            }
        }
        if (j < s.size()) ++j;  // closing quote
    }
    return any;
}

// Preprocessor branches that only exist in hosted builds (#if OSEF_HOSTED,
// #ifndef OSEF_BAREMETAL, #if 0, ... and the #else of the bare-metal forms),
// where the C/C++ standard library is fine.
class HostedOnlyTracker {
public:
    // Feed one source line (from the code view); returns true if the line is
    // compiled in bare-metal builds as far as we can tell.
    bool line(const std::string& raw) {
        std::string t;
        for (char c : raw)
            if (c != ' ' && c != '\t' && c != '\r') t += c;
        if (t.empty() || t[0] != '#') return !hosted_only();
        t.erase(0, 1);
        auto starts = [&](const char* p) { return t.compare(0, std::strlen(p), p) == 0; };
        if (starts("ifdef") || starts("ifndef") || starts("if")) {
            Kind k = Kind::Unknown;
            if (t == "ifOSEF_HOSTED" || t == "if!OSEF_BAREMETAL" || t == "ifndefOSEF_BAREMETAL" ||
                t == "if!defined(OSEF_BAREMETAL)" || t == "if0")
                k = Kind::Hosted;
            else if (t == "ifOSEF_BAREMETAL" || t == "if!OSEF_HOSTED" || t == "ifdefOSEF_BAREMETAL" ||
                     t == "ifdefined(OSEF_BAREMETAL)")
                k = Kind::BareMetal;
            stack_.push_back(k);
        } else if (starts("elif")) {
            if (!stack_.empty()) stack_.back() = Kind::Unknown;
        } else if (starts("else")) {
            if (!stack_.empty() && stack_.back() != Kind::Unknown)
                stack_.back() = stack_.back() == Kind::Hosted ? Kind::BareMetal : Kind::Hosted;
        } else if (starts("endif")) {
            if (!stack_.empty()) stack_.pop_back();
        }
        return !hosted_only();
    }

private:
    enum class Kind { Unknown, Hosted, BareMetal };
    bool hosted_only() const { return std::find(stack_.begin(), stack_.end(), Kind::Hosted) != stack_.end(); }
    std::vector<Kind> stack_;
};

// Headers a freestanding x86_64 kernel has (clang's own resource headers).
bool freestanding_header(const std::string& h) {
    static const char* const ok[] = {"stddef.h", "stdint.h", "stdarg.h", "limits.h", "float.h", "stdbool.h",
                                     "stdalign.h", "stdnoreturn.h", "iso646.h"};
    if (h.compare(0, 5, "osef/") == 0) return true;
    for (const char* o : ok)
        if (h == o) return true;
    return false;
}

// Warns (file:line: warning: ...) about C/C++ standard library use outside
// hosted-only #if blocks: it builds as a hosted app, but the ISO kernel is
// freestanding and fails with "'string' file not found" or similar.
void portability_warnings(const std::string& file, const SourceViews& v) {
    HostedOnlyTracker tracker;
    size_t line_no = 0, begin = 0, warned = 0;
    const char* hint =
        ": the bare-metal (ISO) kernel is freestanding and has no C/C++ standard library; use osef::String, "
        "Vector, Map, format() and c.println() from <osef/osef.h>, or put hosted-only code inside #if OSEF_HOSTED";
    while (begin <= v.code.size() && warned < 10) {
        size_t end = v.code.find('\n', begin);
        if (end == std::string::npos) end = v.code.size();
        ++line_no;
        const std::string code = v.code.substr(begin, end - begin);
        if (tracker.line(code)) {
            const size_t hash = code.find_first_not_of(" \t");
            const size_t inc = code.find("include");
            if (hash != std::string::npos && code[hash] == '#' && inc != std::string::npos) {
                const size_t lt = code.find('<', inc);
                const size_t gt = code.find('>', inc);
                if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
                    const std::string h = code.substr(lt + 1, gt - lt - 1);
                    if (!freestanding_header(h)) {
                        std::cerr << file << ":" << line_no << ": warning: #include <" << h
                                  << "> is not available in the ISO build" << (warned ? " (see above)" : hint) << "\n";
                        ++warned;
                    }
                }
            } else {
                for (size_t p = code.find("std::"); p != std::string::npos; p = code.find("std::", p + 5)) {
                    if (p > 0 && (ident_char(code[p - 1]) || code[p - 1] == ':')) continue;  // osef::std_x, ::std::
                    if (code.compare(p, 21, "std::initializer_list") == 0) continue;     // provided by osef core
                    size_t q = p + 5;
                    while (q < code.size() && ident_char(code[q])) ++q;
                    std::cerr << file << ":" << line_no << ": warning: " << code.substr(p, q - p)
                              << " is not available in the ISO build" << (warned ? " (see above)" : hint) << "\n";
                    ++warned;
                    break;  // one warning per line
                }
            }
        }
        begin = end + 1;
    }
}

int usage_error(const char* msg) {
    std::cerr << "osef-image: error: " << msg << "\n";
    return 2;
}

// check-source [--no-declaration] <file.cpp>
//   main.cpp: must contain OSEF_HARDWARE(...) (outside comments/strings), and
//   a literal .name("...") in it must fit (max kHardwareNameMax - 1 chars).
//   --no-declaration: other OS sources, only the portability warnings.
int cmd_check_source(const Args& a) {
    if (a.positional.size() != 1) return usage_error("usage: osef-image check-source [--no-declaration] <file.cpp>");
    const std::string& file = a.positional[0];
    const SourceViews v = split_source(read_file(file));
    portability_warnings(file, v);
    if (a.no_declaration) return 0;

    const std::string& src = v.code;
    for (size_t pos = src.find("OSEF_HARDWARE"); pos != std::string::npos; pos = src.find("OSEF_HARDWARE", pos + 1)) {
        if (pos > 0 && ident_char(src[pos - 1])) continue;
        size_t j = pos + std::strlen("OSEF_HARDWARE");
        if (j < src.size() && ident_char(src[j])) continue;  // OSEF_HARDWARE_ATTR & co.
        j = skip_space(src, j);
        if (j >= src.size() || src[j] != '(') continue;
        // The declaration's extent: up to the matching ')'.
        size_t close = j;
        for (int depth = 0; close < src.size(); ++close) {
            if (src[close] == '(') ++depth;
            if (src[close] == ')' && --depth == 0) break;
        }
        // .name("...") must fit in HardwareSpec::name. The compiler rejects it
        // too, but with a cascade of constexpr errors; say it in one line here.
        for (size_t k = src.find(".name", j); k != std::string::npos && k < close; k = src.find(".name", k + 5)) {
            size_t p = k + 5;
            if (p < src.size() && ident_char(src[p])) continue;
            p = skip_space(src, p);
            if (p >= src.size() || src[p] != '(') continue;
            std::string name;
            if (!read_string_literal(v.literals, p + 1, name)) continue;  // not a literal: the compiler checks it
            const size_t max = osef::kHardwareNameMax - 1;
            if (name.empty()) {
                std::cerr << file << ":" << line_at(src, k) << ": error: OSEF_HARDWARE: .name(\"\") must not be empty\n";
                return 1;
            }
            if (name.size() > max) {
                std::cerr << file << ":" << line_at(src, k) << ": error: OSEF_HARDWARE: .name(\"" << name << "\") is "
                          << name.size() << " characters long; the maximum is " << max << "\n";
                return 1;
            }
        }
        std::cout << file << ":" << line_at(src, pos) << ": hardware declared\n";
        return 0;
    }
    std::cerr << file << ": error: no hardware declaration found.\n"
              << "Every OSEF operating system must declare its hardware at the top of its main .cpp:\n\n"
              << "    #include <osef/hardware.h>\n\n"
              << "    OSEF_HARDWARE(osef::Hardware()\n"
              << "        .name(\"MyOS\")\n"
              << "        .arch(osef::Arch::x86_64)\n"
              << "        .firmware(osef::Firmware::UEFI)      // or osef::Firmware::BIOS\n"
              << "        .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard));\n\n"
              << "    #include <osef/osef.h>\n";
    return 1;
}

int cmd_inspect(const Args& a) {
    if (a.positional.size() != 1) die("usage: osef-image inspect <kernel.elf>");
    auto s = read_spec(a.positional[0]);
    std::cout << "name=" << s.name << "\n"
              << "slug=" << slug(s.name) << "\n"
              << "arch=" << osef::to_string(osef::Arch(s.arch)) << "\n"
              << "firmware=" << osef::to_string(firmware_of(s)) << "\n"
              << "devices=" << devices_string(s.devices) << "\n"
              << "memory_mb=" << s.memory_mb << "\n"
              << "serial_baud=" << s.serial_baud << "\n"
              << "resolution=" << (s.fb_width ? std::to_string(s.fb_width) + "x" + std::to_string(s.fb_height) : "auto")
              << "\n"
              << "boot_timeout=" << s.boot_timeout_s << "\n";
    return 0;
}

std::string limine_conf(const osef::HardwareSpec& s, const std::string& kernel_name) {
    std::ostringstream c;
    c << "# Generated by osef-image from the OSEF_HARDWARE declaration of \"" << s.name << "\".\n"
      << "# Target: " << osef::to_string(osef::Arch(s.arch)) << ", " << osef::to_string(firmware_of(s))
      << " firmware, devices: " << devices_string(s.devices) << "\n"
      << "timeout: " << s.boot_timeout_s << "\n\n"
      << "/" << s.name << "\n"
      << "    protocol: limine\n"
      << "    path: boot():/boot/" << kernel_name << "\n";
    if (s.fb_width) c << "    resolution: " << s.fb_width << "x" << s.fb_height << "\n";
    return c.str();
}

void copy_into(const fs::path& from, const fs::path& to) {
    if (!fs::exists(from)) die("missing Limine file " + from.string() + " (run tools/bootstrap/limine.sh)");
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

// A staging directory left by an older osef-image that was interrupted while
// xorriso ran (it removed its marker for the duration): only the ISO tree
// (boot/, EFI/) with our generated limine.conf in it.
bool legacy_staging_dir(const fs::path& root) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const std::string n = entry.path().filename().string();
        if (n != "boot" && n != "EFI") return false;
    }
    if (ec) return false;
    std::ifstream conf(root / "boot" / "limine" / "limine.conf");
    std::string first;
    return conf && std::getline(conf, first) && first.rfind("# Generated by osef-image", 0) == 0;
}

int cmd_iso(const Args& a) {
    if (a.positional.size() != 1 || a.out.empty() || a.limine.empty())
        die("usage: osef-image iso <kernel.elf> -o <out.iso> --limine <limine-dir> [--xorriso exe] [--limine-tool exe]");
    const fs::path kernel = a.positional[0];
    const fs::path out = fs::absolute(a.out);
    const fs::path limine = a.limine;
    const auto spec = read_spec(kernel);
    const auto fw = firmware_of(spec);
    const std::string kernel_name = slug(spec.name) + ".elf";

    if (!on_path(a.xorriso)) die("xorriso not found (" + a.xorriso + "); run tools/bootstrap/xorriso.sh or install xorriso");

    // Stage the ISO tree next to the output, in <stem>.iso_root/tree. The
    // marker file <stem>.iso_root/.osef-iso-root says the directory is ours;
    // it sits outside the tree (so it never ends up in the image) and is
    // never removed, so an interrupted run (Ctrl-C during xorriso) cannot
    // leave behind a directory the next run refuses to touch.
    const fs::path root = out.parent_path() / (out.stem().string() + ".iso_root");
    const fs::path marker = root / ".osef-iso-root";
    const fs::path tree = root / "tree";
    if (fs::exists(root) && !fs::exists(marker) && !legacy_staging_dir(root))
        die(root.string() + " exists and was not created by osef-image; remove it or choose another output path");
    fs::create_directories(root);
    if (!fs::exists(marker)) write_file(marker, "staging directory for " + out.filename().string() + "\n");
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.path().filename() != marker.filename()) fs::remove_all(entry.path());
    fs::create_directories(tree / "boot" / "limine");
    fs::copy_file(kernel, tree / "boot" / kernel_name, fs::copy_options::overwrite_existing);
    write_file(tree / "boot" / "limine" / "limine.conf", limine_conf(spec, kernel_name));

    std::vector<std::string> cmd = {a.xorriso, "-as", "mkisofs", "-R", "-r", "-J", "-V", "OSEF"};
    if (fw == osef::Firmware::BIOS) {
        copy_into(limine / "limine-bios.sys", tree / "boot" / "limine" / "limine-bios.sys");
        copy_into(limine / "limine-bios-cd.bin", tree / "boot" / "limine" / "limine-bios-cd.bin");
        // -partition_offset 16: `limine bios-install` needs the MBR partition to
        // start at sector >= 63 so stage 2 fits in front of it.
        cmd.insert(cmd.end(), {"-b", "boot/limine/limine-bios-cd.bin", "-no-emul-boot", "-boot-load-size", "4",
                               "-boot-info-table", "-partition_offset", "16"});
    } else {
        copy_into(limine / "limine-uefi-cd.bin", tree / "boot" / "limine" / "limine-uefi-cd.bin");
        copy_into(limine / "BOOTX64.EFI", tree / "EFI" / "BOOT" / "BOOTX64.EFI");
        cmd.insert(cmd.end(), {"--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part", "--efi-boot-image"});
    }
    cmd.insert(cmd.end(), {"--protective-msdos-label", tree.string(), "-o", out.string()});
    fs::create_directories(out.parent_path());
    if (int rc = run(cmd); rc != 0) die("xorriso failed (exit " + std::to_string(rc) + ")");

    if (fw == osef::Firmware::BIOS) {
        std::string tool = a.limine_tool.empty() ? (limine / "limine").string() : a.limine_tool;
        if (!on_path(tool)) die("limine host tool not found (" + tool + "); run tools/bootstrap/limine.sh");
        if (run({tool, "bios-install", out.string()}) != 0) die("limine bios-install failed");
    }

    std::cout << "osef-image: wrote " << out.string() << "\n"
              << "  os:       " << spec.name << "\n"
              << "  target:   " << osef::to_string(osef::Arch(spec.arch)) << ", " << osef::to_string(fw)
              << " firmware\n"
              << "  devices:  " << devices_string(spec.devices) << "\n"
              << "  memory:   " << spec.memory_mb << " MiB minimum\n";
    return 0;
}

// `run --test` exit status. The kernel writes exit_code + 1 (0..125 -> 1..126)
// or 127 (kernel panic) to the isa-debug-exit port 0xf4, and QEMU then exits
// with (value << 1) | 1, i.e. 3..253 or 255. QEMU's own failures (cannot
// start: missing ISO, bad option, ...) exit with 1, and a VM that powered off
// without writing the port exits with 0: neither decodes to an OS exit code.
constexpr int kRunTimeout = 124;    // killed after --timeout seconds
constexpr int kRunToolError = 126;  // QEMU failed, or no OSEF exit code
constexpr int kRunPanic = 127;      // kernel panic

int cmd_run(const Args& a) {
    if (a.test) g_error_status = kRunToolError;
    if (a.positional.size() != 1 || a.kernel.empty())
        die("usage: osef-image run <image.iso> --kernel <kernel.elf> [--qemu exe] [--ovmf code.fd] [--headless] [--test]");
    if (!fs::is_regular_file(a.positional[0])) die("ISO image " + a.positional[0] + " not found");
    const auto spec = read_spec(a.kernel);
    // UEFI firmware (OVMF) and Limine's UEFI loader keep ~45 MiB for themselves;
    // below ~120 MiB Limine stops with "High memory allocator: Out of memory"
    // before the kernel runs. Give UEFI VMs at least kUefiVmMinMb.
    constexpr uint32_t kUefiVmMinMb = 128;
    uint32_t vm_mb = spec.memory_mb;
    if (firmware_of(spec) == osef::Firmware::UEFI && vm_mb < kUefiVmMinMb) {
        std::cerr << "osef-image: note: " << spec.name << " declares .memory_mb(" << spec.memory_mb
                  << "); running the UEFI VM with " << kUefiVmMinMb << " MiB (UEFI firmware + Limine need it)\n";
        vm_mb = kUefiVmMinMb;
    }
    std::vector<std::string> cmd = {a.qemu, "-M", "q35", "-m", std::to_string(vm_mb), "-cdrom",
                                    a.positional[0], "-boot", "d", "-serial", "stdio"};
    if (firmware_of(spec) == osef::Firmware::UEFI) {
        if (a.ovmf.empty() || !fs::exists(a.ovmf))
            die("UEFI firmware (OVMF/edk2 code .fd) not found; pass --ovmf <path> (QEMU ships share/qemu/edk2-x86_64-code.fd)");
        cmd.insert(cmd.end(), {"-drive", "if=pflash,format=raw,readonly=on,file=" + a.ovmf});
    }
    if (a.headless) cmd.insert(cmd.end(), {"-display", "none"});
    if (a.test) cmd.insert(cmd.end(), {"-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-no-reboot"});
    cmd.insert(cmd.end(), a.passthrough.begin(), a.passthrough.end());
    if (!on_path(a.qemu)) die("QEMU not found (" + a.qemu + "); run tools/bootstrap/qemu.sh or install QEMU");

    int rc = run(cmd, a.timeout);
    if (!a.test) return rc;
    if (rc < 0) return kRunTimeout;
    const int value = (rc & 1) ? rc >> 1 : 0;  // what the kernel wrote to port 0xf4
    if (value == 127) {
        std::cerr << "osef-image: the kernel panicked\n";
        return kRunPanic;
    }
    if (value >= 1) return value - 1;
    if (rc == 1)
        std::cerr << "osef-image: QEMU failed (exit status 1) before the OS could report an exit code\n";
    else
        std::cerr << "osef-image: QEMU exited without an OSEF exit code (status " << rc << ")\n";
    return kRunToolError;
}

void usage() {
    std::cerr << "usage:\n"
                 "  osef-image check-source [--no-declaration] <file.cpp>\n"
                 "  osef-image inspect <kernel.elf>\n"
                 "  osef-image iso <kernel.elf> -o <out.iso> --limine <dir> [--xorriso exe] [--limine-tool exe]\n"
                 "  osef-image run <image.iso> --kernel <kernel.elf> [--qemu exe] [--ovmf code.fd]\n"
                 "                 [--headless] [--test] [--timeout s] [-- extra qemu args]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    Args a = parse(argc, argv, 2);
    if (cmd == "check-source") return cmd_check_source(a);
    if (cmd == "inspect") return cmd_inspect(a);
    if (cmd == "iso") return cmd_iso(a);
    if (cmd == "run") return cmd_run(a);
    usage();
    return 2;
}
