#include "model/project.h"

#include "app/log.h"
#include "model/component_catalog.h"
#include "model/cpp_tokens.h"
#include "model/document.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>

namespace osef::editor {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDeclSourceLabel = "OSEF_HARDWARE declaration";
constexpr const char* kEmDash = "\xE2\x80\x94";
constexpr const char* kBullet = "\xE2\x80\xA2";

// Components whose properties or toggle are bound to the declaration; each
// gets a "main.cpp (OSEF_HARDWARE)" source that follows the declaration.
constexpr const char* kBoundComponents[] = {
    "boot.firmware", "hw.memory", "hw.serial", "hw.display", "hw.keyboard", "hw.rtc", "hw.pci"};

// Truncates to at most `max` bytes without splitting a UTF-8 sequence.
std::string utf8_truncate(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

uint32_t clamp_u32(int64_t v, int64_t lo, int64_t hi) {
    if (hi <= lo) {
        lo = 0;
        hi = std::numeric_limits<uint32_t>::max();
    }
    lo = std::max<int64_t>(lo, 0);
    hi = std::min<int64_t>(hi, std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::clamp(v, lo, hi));
}

std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// ---- Programs / Modules discovered in main.cpp ---------------------------------

struct Registration {
    bool module = false;      // add_module<T>() (else a program)
    bool lambda = false;      // add_fn(...)
    std::string name;         // program name, or the module type's last identifier
    std::string description;  // program description
    std::string type;         // class / module type as written, e.g. "osef::ShellModule"
    int line = 0;
};

std::string last_identifier(std::string_view type) {
    // "osef::ShellModule" -> "ShellModule"; "Foo<Bar>" -> "Foo"
    size_t lt = type.find('<');
    if (lt != std::string_view::npos) type = type.substr(0, lt);
    size_t c = type.rfind("::");
    if (c != std::string_view::npos) type = type.substr(c + 2);
    return std::string(type);
}

// Source text of tokens [a, b) with whitespace collapsed (comments dropped).
std::string join_tokens(const std::vector<cpp::Token>& t, size_t a, size_t b) {
    std::string out;
    for (size_t i = a; i < b; ++i) {
        const cpp::Token& tk = t[i];
        bool word = tk.kind == cpp::TokKind::Identifier || tk.kind == cpp::TokKind::Number;
        if (!out.empty() && word) {
            char last = out.back();
            bool last_word = (last == '_' || (last >= '0' && last <= '9') || (last >= 'a' && last <= 'z') ||
                              (last >= 'A' && last <= 'Z'));
            if (last_word || last == ',') out += ' ';
        }
        out += tk.text;
    }
    return out;
}

// Index of the '>' closing the template argument list opened at t[open], or npos.
size_t match_angle(const std::vector<cpp::Token>& t, size_t open) {
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        const cpp::Token& tk = t[i];
        if (tk.is("<")) ++depth;
        else if (tk.is(">")) {
            if (--depth == 0) return i;
        } else if (tk.is(";") || tk.is("{") || tk.is("}")) {
            return cpp::npos;
        } else if (tk.is("(")) {
            size_t close = cpp::match_bracket(t, i);
            if (close == cpp::npos) return cpp::npos;
            i = close;
        }
    }
    return cpp::npos;
}

struct ScanResult {
    std::vector<Registration> regs;
    std::vector<std::pair<std::string, int>> classes;  // class/struct name -> definition line
};

ScanResult scan_registrations(std::string_view text) {
    ScanResult out;
    cpp::Tokens toks = cpp::tokenize(text);
    const std::vector<cpp::Token>& t = toks.tokens;
    auto string_at = [&](size_t i, std::string& value) {
        if (i >= t.size() || t[i].kind != cpp::TokKind::String) return false;
        value = t[i].value;
        // Adjacent literals concatenate.
        for (size_t j = i + 1; j < t.size() && t[j].kind == cpp::TokKind::String; ++j) value += t[j].value;
        return true;
    };
    auto next_string_arg = [&](size_t i) -> size_t {
        // From a string at i, skip adjacent strings and return the index after the ','.
        while (i < t.size() && t[i].kind == cpp::TokKind::String) ++i;
        return (i < t.size() && t[i].is(",")) ? i + 1 : cpp::npos;
    };
    for (size_t i = 1; i < t.size(); ++i) {
        const cpp::Token& tk = t[i];
        if (tk.kind != cpp::TokKind::Identifier) continue;
        if ((tk.text == "class" || tk.text == "struct") && i + 1 < t.size() &&
            t[i + 1].kind == cpp::TokKind::Identifier) {
            // A definition: `class X final : ...` / `class X {` (not a forward declaration).
            size_t j = i + 2;
            if (j < t.size() && t[j].is("final")) ++j;
            if (j < t.size() && (t[j].is(":") || t[j].is("{"))) {
                std::string name(t[i + 1].text);
                bool known = std::any_of(out.classes.begin(), out.classes.end(),
                                         [&](const auto& c) { return c.first == name; });
                if (!known) out.classes.emplace_back(name, cpp::line_of(text, t[i + 1].begin));
            }
            continue;
        }
        if (!(t[i - 1].is(".") || t[i - 1].is("->"))) continue;
        if (tk.text == "add" || tk.text == "add_module") {
            if (i + 1 >= t.size() || !t[i + 1].is("<")) continue;
            size_t close = match_angle(t, i + 1);
            if (close == cpp::npos || close + 1 >= t.size() || !t[close + 1].is("(") || close == i + 2) continue;
            Registration r;
            r.type = join_tokens(t, i + 2, close);
            r.line = cpp::line_of(text, tk.begin);
            if (tk.text == "add_module") {
                r.module = true;
                r.name = last_identifier(r.type);
                out.regs.push_back(std::move(r));
                continue;
            }
            size_t a = close + 2;
            if (!string_at(a, r.name)) continue;
            size_t d = next_string_arg(a);
            if (d != cpp::npos) string_at(d, r.description);
            out.regs.push_back(std::move(r));
        } else if (tk.text == "add_fn") {
            if (i + 1 >= t.size() || !t[i + 1].is("(")) continue;
            Registration r;
            r.lambda = true;
            r.line = cpp::line_of(text, tk.begin);
            size_t a = i + 2;
            if (!string_at(a, r.name)) continue;
            size_t d = next_string_arg(a);
            if (d != cpp::npos) string_at(d, r.description);
            out.regs.push_back(std::move(r));
        }
    }
    return out;
}

int class_line(const ScanResult& scan, const std::string& name) {
    for (const auto& c : scan.classes)
        if (c.first == name) return c.second;
    return 0;
}

std::unique_ptr<Component> make_program(const Registration& r, const ScanResult& scan) {
    auto c = std::make_unique<Component>();
    c->id = "user.programs." + r.name;
    c->name = r.name;
    c->kind = ComponentKind::Program;
    c->status = ComponentStatus::Implemented;
    c->summary = r.description.empty() ? std::string("Program") : r.description;
    std::string how = r.lambda ? "programs.add_fn(\"" + r.name + "\", ...), a lambda run as its step()"
                               : "programs.add<" + r.type + ">(\"" + r.name + "\", ...)";
    c->description = "Registered in main.cpp at line " + std::to_string(r.line) + " with " + how +
                     ". Run it from the shell by typing `" + r.name + "` (with arguments), or start it from code "
                     "with Context::spawn(\"" + r.name + "\").";
    Property kind;
    kind.id = "kind";
    kind.label = "Kind";
    kind.section = "Program";
    kind.type = PropertyType::Info;
    kind.read_only = true;
    kind.s = r.lambda ? std::string("lambda (add_fn)") : "class " + r.type;
    c->properties.push_back(std::move(kind));
    Property desc;
    desc.id = "description";
    desc.label = "Description";
    desc.section = "Program";
    desc.type = PropertyType::Info;
    desc.read_only = true;
    desc.s = r.description;
    c->properties.push_back(std::move(desc));
    c->sources.push_back(SourceRef{SourceRef::Base::Project, "main.cpp", "Registration", r.line});
    if (!r.lambda) {
        std::string cls = last_identifier(r.type);
        if (int line = class_line(scan, cls))
            c->sources.push_back(SourceRef{SourceRef::Base::Project, "main.cpp", "class " + cls, line});
    }
    return c;
}

std::unique_ptr<Component> make_module(const Registration& r, const ScanResult& scan) {
    auto c = std::make_unique<Component>();
    c->id = "user.modules." + r.name;
    c->name = r.name;
    c->kind = ComponentKind::Module;
    c->status = ComponentStatus::Implemented;
    int def = class_line(scan, r.name);
    const bool shell = r.name == "ShellModule";
    c->summary = shell ? "The OSEF shell (engine module)" : def ? "Module defined in main.cpp" : "Engine module";
    c->description = "Added in main.cpp at line " + std::to_string(r.line) + " with kernel.add_module<" + r.type +
                     ">(). The kernel calls its on_load() at boot, on_tick() every tick and on_unload() at "
                     "shutdown." +
                     (shell ? std::string(" ShellModule registers the program \"sh\".") : std::string());
    Property type;
    type.id = "type";
    type.label = "Type";
    type.section = "Module";
    type.type = PropertyType::Info;
    type.read_only = true;
    type.s = r.type;
    c->properties.push_back(std::move(type));
    c->sources.push_back(SourceRef{SourceRef::Base::Project, "main.cpp", "add_module", r.line});
    if (def) c->sources.push_back(SourceRef{SourceRef::Base::Project, "main.cpp", "class " + r.name, def});
    if (shell) c->sources.push_back(SourceRef{SourceRef::Base::Engine, "engine/src/shell.cpp", "Shell", 0});
    return c;
}

bool same_sources(const std::vector<SourceRef>& a, const std::vector<SourceRef>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].base != b[i].base || a[i].path != b[i].path || a[i].label != b[i].label || a[i].line != b[i].line)
            return false;
    return true;
}

// Replaces `parent`'s children with `fresh`, keeping existing Component
// objects (stable pointers) when the ids are unchanged and in order.
void replace_children(Component& parent, std::vector<std::unique_ptr<Component>> fresh) {
    bool same_ids = fresh.size() == parent.children.size();
    for (size_t i = 0; same_ids && i < fresh.size(); ++i) same_ids = fresh[i]->id == parent.children[i]->id;
    if (same_ids) {
        for (size_t i = 0; i < fresh.size(); ++i) {
            Component& old = *parent.children[i];
            Component& now = *fresh[i];
            old.name = now.name;
            old.summary = now.summary;
            old.description = now.description;
            if (old.properties.size() == now.properties.size()) {
                // Same layout: update values in place (keeps Property pointers valid).
                for (size_t k = 0; k < now.properties.size(); ++k) old.properties[k] = std::move(now.properties[k]);
            } else {
                old.properties = std::move(now.properties);
            }
            if (!same_sources(old.sources, now.sources)) old.sources = std::move(now.sources);
        }
        return;
    }
    parent.children.clear();
    for (auto& c : fresh) parent.add_child(std::move(c));
}

// ---- driver stubs -------------------------------------------------------------------

std::string module_class_name(const std::string& base) {
    std::string out;
    bool upper = true;
    for (char ch : base) {
        bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum) {
            upper = true;
            continue;
        }
        if (upper && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        upper = false;
        out += ch;
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out = "Driver" + out;
    return out + "Module";
}

std::string c_string_body(std::string_view s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        if (ch == '\n') {
            out += "\\n";
            continue;
        }
        out += ch;
    }
    return out;
}

// Word-wraps `text` (paragraphs separated by "\n\n") into "// " comment lines.
std::string comment_block(std::string_view text, std::string_view indent, size_t width) {
    std::string out;
    size_t start = 0;
    bool first_para = true;
    while (start <= text.size()) {
        size_t end = text.find("\n\n", start);
        std::string_view para = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!first_para) out += "//\n";
        first_para = false;
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            while (i < para.size() && (para[i] == ' ' || para[i] == '\n')) ++i;
            size_t j = i;
            while (j < para.size() && para[j] != ' ' && para[j] != '\n') ++j;
            if (j == i) break;
            std::string_view word = para.substr(i, j - i);
            if (!line.empty() && line.size() + 1 + word.size() > width) {
                out += "//" + std::string(indent) + line + "\n";
                line.clear();
            }
            if (!line.empty()) line += ' ';
            line += word;
            i = j;
        }
        if (!line.empty()) out += "//" + std::string(indent) + line + "\n";
        if (end == std::string_view::npos) break;
        start = end + 2;
    }
    return out;
}

std::string driver_stub_text(const Component& c, const std::string& base, const std::string& cls) {
    const std::string device = base + "0";
    std::string s;
    s += "// drivers/" + base + ".cpp: starter driver for " + c.name + " (" + c.id + ").\n";
    s += "// Generated by the OSEF Editor as a starting point: it compiles, but it does\n";
    s += "// not drive any hardware yet.\n";
    s += "//\n";
    s += "// What to implement:\n";
    s += comment_block(c.description.empty() ? c.summary : c.description, "   ", 74);
    s += "//\n";
    s += "// How to enable it:\n";
    s += "//   The OSEF Makefile compiles only <OS dir>/*.cpp, so include this file from\n";
    s += "//   main.cpp, after #include <osef/osef.h>:\n";
    s += "//       #include \"drivers/" + base + ".cpp\"\n";
    s += "//   and add the module in main() before kernel.run():\n";
    s += "//       kernel.add_module<drivers::" + cls + ">();\n";
    s += "//   Everything below is defined inline, so it also links if your build\n";
    s += "//   compiles drivers/*.cpp separately.\n";
    s += "//\n";
    s += "// The engine is freestanding on bare metal: use osef::String, osef::StringView,\n";
    s += "// osef::Vector and friends from <osef/core.h>, never the C++ standard library.\n";
    s += "\n";
    s += "#include <osef/osef.h>\n";
    s += "\n";
    s += "namespace drivers {\n";
    s += "\n";
    s += "class " + cls + " final : public osef::Module {\n";
    s += "public:\n";
    s += "    const char* name() const override { return \"" + c_string_body(base) + "\"; }\n";
    s += "\n";
    s += "    void on_load(osef::Kernel& kernel) override {\n";
    s += "        kernel.log(osef::LogLevel::Info, \"" + c_string_body(base) +
         ": driver stub loaded (not implemented yet)\");\n";
    s += "        // TODO: detect the hardware and initialize it here.\n";
    s += "        // A placeholder device, visible as /dev/" + device + " (read and write are empty for now):\n";
    s += "        kernel.register_device(\"" + c_string_body(device) + "\", {\"" + c_string_body(c.name) +
         " (stub)\", nullptr, nullptr});\n";
    s += "    }\n";
    s += "\n";
    s += "    void on_tick(osef::Kernel& /*kernel*/) override {\n";
    s += "        // TODO: poll the device, or drain events queued by its interrupt handler.\n";
    s += "    }\n";
    s += "};\n";
    s += "\n";
    s += "} // namespace drivers\n";
    return s;
}

std::string shell_quote(const std::string& s) {
#if defined(_WIN32)
    // cmd.exe: double quotes; a path cannot contain '"' on Windows.
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
#endif
}

} // namespace

// ---- Project::Impl --------------------------------------------------------------------

struct Project::Impl {
    std::string name;
    std::string dir_name;
    fs::path directory;
    fs::path main_file;
    fs::path engine_root;
    Document* doc = nullptr;
    Log* log = nullptr;
    std::unique_ptr<Component> root;
    HardwareParse hw;
    OsSettings settings;
    bool has_settings = false;
    DocumentManager* docs = nullptr;
    bool synced = false;
    uint64_t synced_version = 0;

    std::string main_title() const { return main_file.filename().string(); }

    HardwareDecl default_decl() const {
        HardwareDecl d;
        d.name = utf8_truncate(dir_name, 31);
        if (d.name.empty()) d.name = "OSEF";
        d.arch = "x86_64";
        d.firmware = "UEFI";
        d.serial = d.framebuffer = d.ps2_keyboard = true;
        d.memory_mb = 128;
        return d;
    }

    // What the Inspector shows and edits: the declared values, or the
    // defaults a new declaration would get.
    HardwareDecl current_decl() const { return hw.found ? hw.decl : default_decl(); }

    void set_info(const char* component, const char* property, std::string value) {
        if (Component* c = root->find(component))
            if (Property* p = c->property(property)) p->s = std::move(value);
    }

    void refresh_from_decl() {
        const HardwareDecl d = current_decl();
        name = (hw.found && !hw.decl.name.empty()) ? hw.decl.name : dir_name;
        root->name = name;
        if (Property* p = root->property("name")) p->s = d.name;

        if (Component* c = root->find("boot.firmware")) {
            if (Property* p = c->property("firmware")) p->i = d.firmware == "BIOS" ? 1 : d.firmware == "UEFI" ? 2 : 0;
            if (Property* p = c->property("arch")) p->i = d.arch == "x86_64" ? 1 : 0;
            if (Property* p = c->property("boot_timeout")) p->i = d.boot_timeout_s;
            if (Property* p = c->property("image"))
                p->s = d.firmware == "BIOS"   ? "BIOS-only ISO (El Torito no-emulation boot)"
                       : d.firmware == "UEFI" ? "UEFI-only ISO (EFI system partition)"
                                              : "choose a firmware";
            c->summary = d.firmware.empty() ? "Limine boots the kernel via BIOS or UEFI"
                                            : "Limine boots the kernel via " + d.firmware;
        }
        if (Component* c = root->find("hw.cpu")) {
            if (Property* p = c->property("arch"))
                p->s = d.arch == "x86_64" ? "x86_64 (long mode, higher-half kernel)"
                       : d.arch.empty()   ? "not declared"
                                          : d.arch + " (unsupported)";
        }
        if (Component* c = root->find("hw.memory"))
            if (Property* p = c->property("memory_mb")) p->i = d.memory_mb;
        if (Component* c = root->find("hw.serial")) {
            c->enabled = d.serial;
            if (Property* p = c->property("baud")) {
                p->i = d.serial_baud;
                bool listed = std::any_of(p->options.begin(), p->options.end(),
                                          [&](const EnumOption& o) { return o.value == p->i; });
                if (!listed) {
                    // A rate the code uses but the list doesn't have (e.g. 28800).
                    p->options.push_back({std::to_string(d.serial_baud), d.serial_baud, "Declared in main.cpp"});
                    std::sort(p->options.begin(), p->options.end(),
                              [](const EnumOption& a, const EnumOption& b) { return a.value > b.value; });
                }
            }
        }
        if (Component* c = root->find("hw.display")) {
            c->enabled = d.framebuffer;
            if (Property* p = c->property("resolution")) {
                p->i = d.fb_width;
                p->i2 = d.fb_height;
            }
        }
        if (Component* c = root->find("hw.keyboard")) c->enabled = d.ps2_keyboard;
        if (Component* c = root->find("hw.rtc")) c->enabled = d.rtc;
        if (Component* c = root->find("hw.pci")) c->enabled = d.pci;

        // Declaration status + where it is.
        std::string status;
        if (!hw.found) {
            status = std::string("Missing ") + kEmDash + " add one from the Inspector";
        } else {
            status = "Declared at " + main_title() + ":" + std::to_string(hw.line);
            if (!hw.problems.empty()) {
                status += ", " + plural(hw.problems.size(), "problem", "problems") + ":";
                for (const std::string& p : hw.problems) status += "\n" + std::string(kBullet) + " " + p;
            }
        }
        set_info("os", "declaration_status", std::move(status));
        const int line = hw.found ? hw.line : 0;
        if (!root->sources.empty()) root->sources.front().line = line;
        for (const char* id : kBoundComponents) {
            if (Component* c = root->find(id))
                for (SourceRef& s : c->sources)
                    if (s.base == SourceRef::Base::Project && s.label == kDeclSourceLabel) s.line = line;
        }
    }

    void refresh_children(std::string_view text) {
        ScanResult scan = scan_registrations(text);
        std::vector<std::unique_ptr<Component>> programs, modules;
        for (const Registration& r : scan.regs) {
            auto& list = r.module ? modules : programs;
            auto c = r.module ? make_module(r, scan) : make_program(r, scan);
            auto dup = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e->id == c->id; });
            if (dup != list.end()) {
                // Programs: a later add() replaces the earlier one (as in the engine).
                // Modules: keep the first.
                if (!r.module) *dup = std::move(c);
                continue;
            }
            list.push_back(std::move(c));
        }
        if (Component* p = root->find("user.programs")) {
            set_info("user.programs", "count", std::to_string(programs.size()));
            p->summary = programs.empty() ? "Programs registered in main.cpp"
                                          : plural(programs.size(), "program", "programs") + " registered in " +
                                                main_title();
            replace_children(*p, std::move(programs));
        }
        if (Component* m = root->find("user.modules")) {
            set_info("user.modules", "count", std::to_string(modules.size()));
            m->summary = modules.empty() ? "Engine plugins added with add_module"
                                         : plural(modules.size(), "module", "modules") + " added in " + main_title();
            replace_children(*m, std::move(modules));
        }
    }

    // Writes `d` into the main document as one undoable edit.
    void write_decl(const HardwareDecl& d) {
        if (!doc) return;
        if (doc->read_only()) {
            if (log) log->warn(main_title() + " is read-only; the hardware declaration was not changed");
            resync();
            return;
        }
        std::string text = doc->text();
        if (hw.found && hardware_decl_has_unparsed_code(text)) {
            if (log)
                log->warn(main_title() + ":" + std::to_string(hw.line) +
                          ": the OSEF_HARDWARE declaration contains code the editor cannot read (see Declaration "
                          "status on the OS). Fix it in the code first; the Inspector did not change it.");
            resync();
            return;
        }
        std::string next = apply_hardware_decl(text, d);
        if (next != text) doc->replace_all(next);
        resync();
    }

    void resync() { synced = false; }
};

// ---- Project --------------------------------------------------------------------------

Project::Project() : impl_(std::make_unique<Impl>()) {}
Project::~Project() = default;

std::unique_ptr<Project> Project::open(const fs::path& location, const fs::path& engine_root,
                                       DocumentManager& docs, Log& log, std::string* error) {
    auto fail = [&](const std::string& message) -> std::unique_ptr<Project> {
        if (error) *error = message;
        return nullptr;
    };
    std::error_code ec;
    fs::path loc = fs::absolute(location, ec);
    if (ec) return fail("cannot resolve " + location.string() + ": " + ec.message());
    fs::path main_file, directory;
    if (fs::is_directory(loc, ec)) {
        directory = loc;
        main_file = loc / "main.cpp";
        if (!fs::is_regular_file(main_file, ec)) return fail(loc.string() + " has no main.cpp");
    } else if (fs::is_regular_file(loc, ec)) {
        if (loc.extension() != ".cpp") return fail(loc.string() + " is not a .cpp file");
        main_file = loc;
        directory = loc.parent_path();
    } else {
        return fail(loc.string() + ": no such file or directory");
    }
    fs::path root = fs::absolute(engine_root, ec);
    if (ec || !fs::is_regular_file(root / "engine" / "include" / "osef" / "hardware.h", ec))
        return fail(engine_root.string() + " is not an OSEF checkout (engine/include/osef/hardware.h is missing)");

    auto canon = [](const fs::path& p) {
        std::error_code e;
        fs::path c = fs::weakly_canonical(p, e);
        return e ? p.lexically_normal() : c;
    };
    std::string open_error;
    Document* doc = docs.open(main_file, &open_error);
    if (!doc) return fail("cannot open " + main_file.string() + (open_error.empty() ? "" : ": " + open_error));

    std::unique_ptr<Project> p(new Project());
    Impl& m = *p->impl_;
    m.directory = canon(directory);
    m.main_file = canon(main_file);
    m.engine_root = canon(root);
    m.doc = doc;
    m.log = &log;
    m.docs = &docs;
    if (fs::is_regular_file(m.directory / kSettingsFile, ec)) {
        std::string settings_error;
        if (load_settings(m.directory / kSettingsFile, m.settings, &settings_error)) m.has_settings = true;
        else log.warn("Cannot read " + std::string(kSettingsFile) + ": " + settings_error);
    }
    m.dir_name = m.directory.filename().string();
    if (m.dir_name.empty()) m.dir_name = "OSEF";
    m.name = m.dir_name;
    m.root = build_component_catalog(m.name);

    // Project facts and the links to the declaration.
    m.set_info("os", "project_dir", m.directory.string());
    m.set_info("os", "main_file", m.main_title());
    if (!m.root->sources.empty()) {
        m.root->sources.front().path = m.main_title();
        m.root->sources.front().label = "OS entry point and hardware declaration";
    }
    for (const char* id : {"user.programs", "user.modules"})
        if (Component* c = m.root->find(id))
            for (SourceRef& s : c->sources)
                if (s.base == SourceRef::Base::Project) s.path = m.main_title();
    for (const char* id : kBoundComponents)
        if (Component* c = m.root->find(id))
            c->sources.push_back(SourceRef{SourceRef::Base::Project, m.main_title(), kDeclSourceLabel, 0});

    p->sync_from_code();

    log.info("Opened " + m.name + " (" + m.main_file.string() + ")");
    if (!m.hw.found) {
        log.warn(m.main_title() + " has no OSEF_HARDWARE declaration; add one from the Inspector (select " + m.name +
                 ")");
    } else {
        for (const std::string& problem : m.hw.problems) log.warn(m.main_title() + ": " + problem);
    }
    return p;
}

OsSettings& Project::settings() { return impl_->settings; }
bool Project::uses_settings() const { return impl_->has_settings; }
std::vector<std::string> Project::settings_problems() const { return validate_settings(impl_->settings); }

bool Project::settings_changed(std::string* error) {
    Impl& m = *impl_;
    std::string err;
    if (!write_settings_files(m.directory, m.settings, &err)) {
        if (m.log) m.log->error("Cannot save the OS settings: " + err);
        if (error) *error = err;
        return false;
    }
    m.has_settings = true;
    // Keep an open copy of the generated file in sync (it is rewritten on disk).
    if (m.docs)
        if (Document* gen = m.docs->find(m.directory / kGeneratedFile))
            if (!gen->modified()) gen->reload(nullptr);
    return true;
}

const std::string& Project::name() const { return impl_->name; }
const fs::path& Project::directory() const { return impl_->directory; }
const fs::path& Project::main_file() const { return impl_->main_file; }
const fs::path& Project::engine_root() const { return impl_->engine_root; }
Document* Project::main_document() const { return impl_->doc; }
Component& Project::root() { return *impl_->root; }
Component* Project::find(std::string_view component_id) { return impl_->root->find(component_id); }
const HardwareParse& Project::hardware() const { return impl_->hw; }

void Project::sync_from_code() {
    Impl& m = *impl_;
    if (!m.doc) return;
    const uint64_t version = m.doc->version();
    if (m.synced && version == m.synced_version) return;
    const std::string text = m.doc->text();
    m.hw = parse_hardware_decl(text);
    m.synced = true;
    m.synced_version = version;
    m.refresh_from_decl();
    m.refresh_children(text);
}

void Project::apply_property(Component& component, const Property& edited) {
    Impl& m = *impl_;
    // Copy first: `edited` usually lives in the component, and syncing below
    // refreshes bound properties from the code.
    const Property property = edited;
    const std::string cid = component.id;
    const std::string& pid = property.id;
    sync_from_code();  // edit what the code says now
    HardwareDecl d = m.current_decl();
    bool handled = true;
    if (cid == "os" && pid == "name") {
        d.name = property.s;
    } else if (cid == "boot.firmware" && pid == "firmware") {
        d.firmware = property.i == 1 ? "BIOS" : property.i == 2 ? "UEFI" : "";
    } else if (cid == "boot.firmware" && pid == "arch") {
        d.arch = property.i == 1 ? "x86_64" : "";
    } else if (cid == "boot.firmware" && pid == "boot_timeout") {
        d.boot_timeout_s = clamp_u32(property.i, property.min, property.max);
    } else if (cid == "hw.memory" && pid == "memory_mb") {
        d.memory_mb = clamp_u32(property.i, property.min, property.max);
    } else if (cid == "hw.serial" && pid == "baud") {
        if (property.i <= 0 || property.i > 115200 || 115200 % property.i != 0) {
            if (m.log) m.log->warn("Baud rate " + std::to_string(property.i) + " does not divide 115200; not applied");
            m.resync();
            sync_from_code();
            return;
        }
        d.serial_baud = static_cast<uint32_t>(property.i);
    } else if (cid == "hw.display" && pid == "resolution") {
        d.fb_width = clamp_u32(property.i, property.min, property.max);
        d.fb_height = clamp_u32(property.i2, property.min, property.max);
    } else {
        handled = false;
    }
    if (!handled) return;
    m.write_decl(d);
    sync_from_code();
}

void Project::apply_enabled(Component& component) {
    Impl& m = *impl_;
    const bool enabled = component.enabled;
    const std::string id = component.id;
    sync_from_code();
    HardwareDecl d = m.current_decl();
    if (id == "hw.serial") d.serial = enabled;
    else if (id == "hw.display") d.framebuffer = enabled;
    else if (id == "hw.keyboard") d.ps2_keyboard = enabled;
    else if (id == "hw.rtc") d.rtc = enabled;
    else if (id == "hw.pci") d.pci = enabled;
    else return;
    m.write_decl(d);
    sync_from_code();
}

void Project::add_hardware_declaration() {
    Impl& m = *impl_;
    sync_from_code();
    if (m.hw.found) {
        if (m.log) m.log->info(m.main_title() + " already declares its hardware (line " + std::to_string(m.hw.line) + ")");
        return;
    }
    m.write_decl(m.default_decl());
    sync_from_code();
    if (m.log && m.hw.found)
        m.log->info("Added an OSEF_HARDWARE declaration to " + m.main_title() + " at line " +
                    std::to_string(m.hw.line) + " (not saved yet)");
}

fs::path Project::resolve(const SourceRef& ref) const {
    fs::path p(ref.path);
    if (p.is_absolute()) return p.lexically_normal();
    const fs::path& base = ref.base == SourceRef::Base::Engine ? impl_->engine_root : impl_->directory;
    return (base / p).lexically_normal();
}

std::vector<BuildTarget> Project::build_targets() const {
    const Impl& m = *impl_;
#if defined(_WIN32)
    const std::string dir = shell_quote(m.directory.generic_string());
#else
    const std::string dir = shell_quote(m.directory.string());
#endif
    const std::string os_dir = "OS_DIR=" + dir;
    return {
        {"run", "Run (hosted)", "Build " + m.name + " as a native app and run it in the console",
         "make run " + os_dir, m.engine_root},
        {"iso", "Build ISO", "Build the bare-metal kernel and a bootable ISO for the declared firmware",
         "make iso " + os_dir, m.engine_root},
        {"qemu", "Run in QEMU", "Build the ISO and boot it in QEMU with the declared firmware and memory",
         "make run-iso " + os_dir, m.engine_root},
        {"test", "Run Tests", "Build and run the OSEF engine tests", "make test", m.engine_root},
    };
}

fs::path Project::create_driver_stub(const Component& component, std::string* error) {
    const Impl& m = *impl_;
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return fs::path();
    };
    switch (component.kind) {
    case ComponentKind::Os:
    case ComponentKind::Group:
    case ComponentKind::Programs:
    case ComponentKind::Program:
    case ComponentKind::Modules:
    case ComponentKind::Module:
        return fail(component.name + " is not a device; driver stubs are for hardware components");
    default: break;
    }
    if (component.status == ComponentStatus::Implemented)
        return fail(component.name + " is already implemented by the engine (see its source files)");
    std::string base = component.id;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(dot + 1);
    if (base.empty()) return fail("component has no id");
    const fs::path dir = m.directory / "drivers";
    const fs::path path = dir / (base + ".cpp");
    std::error_code ec;
    if (fs::exists(path, ec)) return fail("drivers/" + base + ".cpp already exists; not overwriting it");
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());
    const std::string text = driver_stub_text(component, base, module_class_name(base));
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) return fail("cannot write " + path.string());
        out << text;
        out.flush();
        if (!out) {
            out.close();
            fs::remove(path, ec);
            return fail("cannot write " + path.string());
        }
    }
    return path;  // callers (Inspector, Hierarchy, menus) log and open it
}

} // namespace osef::editor
