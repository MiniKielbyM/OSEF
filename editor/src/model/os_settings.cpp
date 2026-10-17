#include "model/os_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>

namespace osef::editor {

namespace fs = std::filesystem;

// ================================================================ catalogs

const std::vector<ProgramTemplate>& program_templates() {
    static const std::vector<ProgramTemplate> templates = {
        {"message", "Show a message", "Prints a message and finishes.", "Message", "Hello there!", "", 0, 0, 0},
        {"countdown", "Countdown", "Counts down one number per second, then shows a message.", "Message at the end",
         "Lift off!", "Start from (seconds)", 10, 1, 3600},
        {"clock", "Clock", "Shows the date and time once per second (needs the Real-Time Clock).", "", "",
         "How many times", 5, 1, 3600},
        {"guess", "Guess the number", "A small game: the OS picks a number and the player guesses it.", "", "",
         "Highest number", 100, 2, 1000000},
        {"ask", "Ask a question", "Asks a question and greets the answer.", "Question", "What is your name?", "", 0,
         0, 0},
    };
    return templates;
}

const ProgramTemplate* find_program_template(std::string_view kind) {
    for (const auto& t : program_templates())
        if (t.kind == kind) return &t;
    return nullptr;
}

const std::vector<std::string>& color_names() {
    static const std::vector<std::string> names = {
        "default",      "black",         "red",           "green",       "yellow",         "blue",
        "magenta",      "cyan",          "white",         "bright-black", "bright-red",    "bright-green",
        "bright-yellow", "bright-blue", "bright-magenta", "bright-cyan",  "bright-white",
    };
    return names;
}

std::string color_enum_name(std::string_view color) {
    if (std::find(color_names().begin(), color_names().end(), color) == color_names().end()) return {};
    std::string out;
    bool upper = true;
    for (char c : color) {
        if (c == '-') {
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out;
}

const std::vector<std::string>& reserved_command_names() {
    static const std::vector<std::string> names = {
        "help", "echo", "time", "irq", "int", "lspci", "ls",   "cat",   "cd",    "pwd",  "mkdir",  "touch",    "rm",
        "mounts", "ps", "kill", "send",  "services", "sys", "mods", "sched", "hw", "mem", "uptime", "uname",
        "clear", "exit", "shutdown", "sh",
    };
    return names;
}

bool is_valid_command_name(std::string_view name) {
    if (name.empty() || name.size() > 32 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

std::vector<std::string> validate_settings(const OsSettings& s) {
    std::vector<std::string> problems;
    std::set<std::string> seen;
    const auto& reserved = reserved_command_names();
    for (const ProgramSpec& p : s.programs) {
        const std::string shown = p.name.empty() ? "(unnamed)" : "'" + p.name + "'";
        if (!find_program_template(p.kind)) problems.push_back("Program " + shown + " has an unknown type '" + p.kind + "'.");
        if (!is_valid_command_name(p.name))
            problems.push_back("Program " + shown +
                               ": a command name must start with a lowercase letter and use only a-z, 0-9, - and _.");
        else if (std::find(reserved.begin(), reserved.end(), p.name) != reserved.end())
            problems.push_back("Program " + shown + " has the same name as a built-in command.");
        else if (!seen.insert(p.name).second)
            problems.push_back("There are two programs called " + shown + ".");
    }
    if (s.start_program != "sh") {
        bool found = std::any_of(s.programs.begin(), s.programs.end(),
                                 [&](const ProgramSpec& p) { return p.name == s.start_program; });
        if (!found) problems.push_back("The start program '" + s.start_program + "' does not exist.");
    } else if (!s.shell) {
        problems.push_back("The shell is turned off, so choose another program to start at boot.");
    }
    for (const InterruptSpec& i : s.interrupts) {
        const std::string shown = "Interrupt '" + (i.name.empty() ? std::string("(unnamed)") : i.name) + "'";
        if (i.source == "vector") {
            if (i.number < 48 || i.number > 255)
                problems.push_back(shown + ": OS vectors are 48-255 (0x30-0xFF); 0-31 are CPU exceptions, 32-47 IRQ lines.");
        } else if (i.source == "irq") {
            if (i.number < 0 || i.number > 15) problems.push_back(shown + ": IRQ lines are 0-15.");
        } else {
            problems.push_back(shown + ": source must be 'vector' or 'irq'.");
        }
        if (i.action == "program") {
            const bool exists = i.text == "sh" || std::any_of(s.programs.begin(), s.programs.end(),
                                                               [&](const ProgramSpec& p) { return p.name == i.text; });
            if (!exists) problems.push_back(shown + ": program '" + i.text + "' does not exist.");
        } else if (i.action != "message") {
            problems.push_back(shown + ": action must be 'message' or 'program'.");
        }
    }
    for (int a = 0; a < static_cast<int>(s.interrupts.size()); ++a)
        for (int b = a + 1; b < static_cast<int>(s.interrupts.size()); ++b)
            if (s.interrupts[a].source == s.interrupts[b].source && s.interrupts[a].number == s.interrupts[b].number)
                problems.push_back("Two interrupt handlers use the same " + s.interrupts[a].source + " " +
                                   std::to_string(s.interrupts[a].number) + ".");
    for (const FileSpec& f : s.files)
        if (f.path.size() < 2 || f.path[0] != '/' || f.path.back() == '/')
            problems.push_back("File '" + f.path + "': the path must start with / and name a file, like /home/notes.txt.");
    for (const std::string* c : {&s.text_color, &s.background, &s.prompt_name_color, &s.prompt_path_color})
        if (color_enum_name(*c).empty()) problems.push_back("Unknown color '" + *c + "'.");
    if (s.text_size < 0 || s.text_size > 4) problems.push_back("Text size must be between 0 (automatic) and 4.");
    return problems;
}

// ================================================================ ini storage

namespace {

// Leading/trailing spaces are written as "\s" so trimming can't eat them
// (prompts usually end with one).
std::string escape_value(std::string_view v) {
    size_t lead = 0, trail = 0;
    while (lead < v.size() && v[lead] == ' ') ++lead;
    while (trail < v.size() - lead && v[v.size() - 1 - trail] == ' ') ++trail;
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c == ' ' && (i < lead || i >= v.size() - trail)) {
            out += "\\s";
            continue;
        }
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': break;
        default: out += c;
        }
    }
    return out;
}

std::string unescape_value(std::string_view v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 == v.size()) {
            out += v[i];
            continue;
        }
        const char n = v[++i];
        out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 's' ? ' ' : n;
    }
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

bool parse_bool(const std::string& v) { return v == "yes" || v == "true" || v == "on" || v == "1"; }

int parse_int(const std::string& v, int fallback) {
    try {
        size_t used = 0;
        int n = std::stoi(v, &used);
        return used == v.size() ? n : fallback;
    } catch (...) {
        return fallback;
    }
}

void kv(std::string& out, const char* key, std::string_view value) {
    out += key;
    out += " = ";
    out += escape_value(value);
    out += '\n';
}

} // namespace

std::string serialize_settings(const OsSettings& s) {
    std::string out =
        "# OSEF settings, edited by the OSEF editor (Settings panel).\n"
        "# The editor turns them into osef_generated.cpp; edit either here or in the editor.\n\n"
        "[appearance]\n";
    kv(out, "welcome", s.welcome);
    kv(out, "prompt", s.prompt);
    kv(out, "text_color", s.text_color);
    kv(out, "background", s.background);
    kv(out, "prompt_name_color", s.prompt_name_color);
    kv(out, "prompt_path_color", s.prompt_path_color);
    kv(out, "text_size", std::to_string(s.text_size));
    out += "\n[features]\n";
    kv(out, "shell", s.shell ? "yes" : "no");
    kv(out, "start_program", s.start_program);
    for (const ProgramSpec& p : s.programs) {
        out += "\n[program]\n";
        kv(out, "kind", p.kind);
        kv(out, "name", p.name);
        kv(out, "description", p.description);
        kv(out, "text", p.text);
        kv(out, "number", std::to_string(p.number));
    }
    for (const InterruptSpec& i : s.interrupts) {
        out += "\n[interrupt]\n";
        kv(out, "source", i.source);
        kv(out, "number", std::to_string(i.number));
        kv(out, "name", i.name);
        kv(out, "action", i.action);
        kv(out, "text", i.text);
    }
    for (const FileSpec& f : s.files) {
        out += "\n[file]\n";
        kv(out, "path", f.path);
        kv(out, "content", f.content);
    }
    return out;
}

OsSettings parse_settings(std::string_view text, std::vector<std::string>* warnings) {
    OsSettings s;
    std::string section;
    std::istringstream in{std::string(text)};
    std::string raw;
    int line_no = 0;
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back("line " + std::to_string(line_no) + ": " + w);
    };
    while (std::getline(in, raw)) {
        ++line_no;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section == "program") s.programs.emplace_back();
            else if (section == "file") s.files.emplace_back();
            else if (section == "interrupt") s.interrupts.emplace_back();
            else if (section != "appearance" && section != "features") warn("unknown section [" + section + "]");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn("expected key = value");
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, eq));
        const std::string value = unescape_value(trim(std::string_view(line).substr(eq + 1)));
        bool known = true;
        if (section == "appearance") {
            if (key == "welcome") s.welcome = value;
            else if (key == "prompt") s.prompt = value;
            else if (key == "text_color") s.text_color = value;
            else if (key == "background") s.background = value;
            else if (key == "prompt_name_color") s.prompt_name_color = value;
            else if (key == "prompt_path_color") s.prompt_path_color = value;
            else if (key == "text_size") s.text_size = parse_int(value, 0);
            else known = false;
        } else if (section == "features") {
            if (key == "shell") s.shell = parse_bool(value);
            else if (key == "start_program") s.start_program = value;
            else known = false;
        } else if (section == "program") {
            ProgramSpec& p = s.programs.back();
            if (key == "kind") p.kind = value;
            else if (key == "name") p.name = value;
            else if (key == "description") p.description = value;
            else if (key == "text") p.text = value;
            else if (key == "number") p.number = parse_int(value, p.number);
            else known = false;
        } else if (section == "interrupt") {
            InterruptSpec& i = s.interrupts.back();
            if (key == "source") i.source = value;
            else if (key == "number") i.number = parse_int(value, i.number);
            else if (key == "name") i.name = value;
            else if (key == "action") i.action = value;
            else if (key == "text") i.text = value;
            else known = false;
        } else if (section == "file") {
            FileSpec& f = s.files.back();
            if (key == "path") f.path = value;
            else if (key == "content") f.content = value;
            else known = false;
        } else {
            known = false;
        }
        if (!known) warn("unknown setting '" + key + "'" + (section.empty() ? "" : " in [" + section + "]"));
    }
    return s;
}

namespace {

bool read_text(const fs::path& file, std::string& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read " + file.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Writes only when the content changed (keeps build timestamps stable).
bool write_if_changed(const fs::path& file, const std::string& content, std::string* error) {
    std::string old;
    if (fs::exists(file) && read_text(file, old, nullptr) && old == content) return true;
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << content)) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "cannot replace " + file.string();
        return false;
    }
    return true;
}

} // namespace

bool load_settings(const fs::path& file, OsSettings& out, std::string* error) {
    std::string text;
    if (!read_text(file, text, error)) return false;
    out = parse_settings(text);
    return true;
}

bool save_settings(const fs::path& file, const OsSettings& settings, std::string* error) {
    return write_if_changed(file, serialize_settings(settings), error);
}

// ================================================================ code generation

namespace {

// A C++ string literal (UTF-8 passes through unchanged).
std::string literal(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02x\"\"", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

// "guess-the-number" -> "GuessTheNumber"
std::string class_name(std::string_view command) {
    std::string out;
    bool upper = true;
    for (char c : command) {
        if (c == '-' || c == '_') {
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out + "Program";
}

std::string color(std::string_view name) {
    std::string e = color_enum_name(name);
    return "osef::ConsoleColor::" + (e.empty() ? std::string("Default") : e);
}

int clamp_number(const ProgramSpec& p) {
    const ProgramTemplate* t = find_program_template(p.kind);
    if (!t || t->number_label.empty()) return p.number;
    return std::max(t->number_min, std::min(t->number_max, p.number));
}

std::string program_class(const ProgramSpec& p) {
    const std::string cls = class_name(p.name);
    const int n = clamp_number(p);
    std::ostringstream o;
    o << "// " << (find_program_template(p.kind) ? find_program_template(p.kind)->label : p.kind) << ": " << p.name
      << "\n";
    if (p.kind == "message") {
        o << "class " << cls << " final : public osef::Process {\n"
          << "    void step(osef::Context& c) override {\n"
          << "        c.println(" << literal(p.text) << ");\n"
          << "        c.exit(0);\n"
          << "    }\n};\n";
    } else if (p.kind == "countdown") {
        o << "class " << cls << " final : public osef::Process {\n"
          << "    int left_ = " << n << ";\n"
          << "    void step(osef::Context& c) override {\n"
          << "        if (left_ == 0) {\n"
          << "            c.println(" << literal(p.text) << ");\n"
          << "            c.exit(0);\n"
          << "            return;\n"
          << "        }\n"
          << "        c.println(osef::to_string(left_--));\n"
          << "        c.sleep(1000);\n"
          << "    }\n};\n";
    } else if (p.kind == "clock") {
        o << "class " << cls << " final : public osef::Process {\n"
          << "    int left_ = " << n << ";\n"
          << "    void step(osef::Context& c) override {\n"
          << "        osef::WallClock t;\n"
          << "        if (!c.kernel().platform().read_wall_clock(t)) {\n"
          << "            c.println(\"" << p.name << ": no real-time clock (turn on Hardware > Real-Time Clock)\");\n"
          << "            c.exit(1);\n"
          << "            return;\n"
          << "        }\n"
          << "        c.println(osef::format(\"%04u-%02u-%02u %02u:%02u:%02u\", unsigned(t.year), unsigned(t.month),\n"
          << "                               unsigned(t.day), unsigned(t.hour), unsigned(t.minute), unsigned(t.second)));\n"
          << "        if (--left_ <= 0) c.exit(0);\n"
          << "        else c.sleep(1000);\n"
          << "    }\n};\n";
    } else if (p.kind == "guess") {
        o << "class " << cls << " final : public osef::Process {\n"
          << "    long long secret_ = 0;\n"
          << "    int tries_ = 0;\n"
          << "    void on_start(osef::Context& c) override {\n"
          << "        secret_ = static_cast<long long>(c.kernel().platform().random_u64() % " << n << "u) + 1;\n"
          << "        c.println(\"I'm thinking of a number from 1 to " << n << ". Your guess?\");\n"
          << "    }\n"
          << "    void step(osef::Context& c) override {\n"
          << "        osef::String line;\n"
          << "        if (!c.read_line(line)) {\n"
          << "            if (c.input_eof()) c.exit(1);\n"
          << "            return;  // waiting for the player\n"
          << "        }\n"
          << "        long long guess = 0;\n"
          << "        if (!osef::parse_int(line, guess)) {\n"
          << "            c.println(\"Please type a number.\");\n"
          << "            return;\n"
          << "        }\n"
          << "        ++tries_;\n"
          << "        if (guess < secret_) c.println(\"Higher!\");\n"
          << "        else if (guess > secret_) c.println(\"Lower!\");\n"
          << "        else {\n"
          << "            c.println(osef::format(\"You got it in %d %s!\", tries_, tries_ == 1 ? \"try\" : \"tries\"));\n"
          << "            c.exit(0);\n"
          << "        }\n"
          << "    }\n};\n";
    } else if (p.kind == "ask") {
        o << "class " << cls << " final : public osef::Process {\n"
          << "    void on_start(osef::Context& c) override { c.println(" << literal(p.text) << "); }\n"
          << "    void step(osef::Context& c) override {\n"
          << "        osef::String answer;\n"
          << "        if (!c.read_line(answer)) {\n"
          << "            if (c.input_eof()) c.exit(1);\n"
          << "            return;  // waiting for an answer\n"
          << "        }\n"
          << "        c.println(\"Nice to meet you, \" + answer + \"!\");\n"
          << "        c.exit(0);\n"
          << "    }\n};\n";
    }
    return o.str();
}

} // namespace

std::string generate_settings_code(const OsSettings& s) {
    std::ostringstream o;
    o << "// Generated by the OSEF editor from osef-settings.ini. Do not edit this file:\n"
         "// change the settings in the editor (or in osef-settings.ini) and it is rewritten.\n"
         "//\n"
         "// It adds one module to the OS automatically (OSEF_AUTO_MODULE), so main.cpp\n"
         "// needs no extra code.\n\n"
         "#include <osef/osef.h>\n\n"
         "namespace osef_generated {\n\n";

    std::vector<const ProgramSpec*> programs;
    std::set<std::string> names;
    for (const ProgramSpec& p : s.programs)
        if (find_program_template(p.kind) && is_valid_command_name(p.name) && names.insert(p.name).second)
            programs.push_back(&p);  // invalid entries are skipped (the editor lists them as problems)

    for (const ProgramSpec* p : programs) o << program_class(*p) << "\n";

    o << "class EditorSettings final : public osef::Module {\n"
         "public:\n"
         "    const char* name() const override { return \"editor-settings\"; }\n\n"
         "    void on_configure(osef::KernelConfig& config) override {\n"
         "        osef::Appearance& a = config.appearance;\n"
      << "        a.welcome = " << literal(s.welcome) << ";\n"
      << "        a.prompt = " << literal(s.prompt) << ";\n"
      << "        a.text = " << color(s.text_color) << ";\n"
      << "        a.background = " << color(s.background) << ";\n"
      << "        a.prompt_name = " << color(s.prompt_name_color) << ";\n"
      << "        a.prompt_path = " << color(s.prompt_path_color) << ";\n"
      << "        a.text_size = " << std::max(0, std::min(4, s.text_size)) << ";\n";
    if (s.start_program != "sh" && names.count(s.start_program))
        o << "        config.init_program = " << literal(s.start_program) << ";\n";
    o << "    }\n\n"
         "    void on_load(osef::Kernel& kernel) override {\n";
    if (s.shell)
        o << "        if (!kernel.find_module<osef::ShellModule>()) kernel.add_module<osef::ShellModule>();\n";
    for (const FileSpec& f : s.files) {
        if (f.path.size() < 2 || f.path[0] != '/' || f.path.back() == '/') continue;
        // Text files end with a newline, so `cat` and the motd don't run into the prompt.
        const std::string content =
            f.content.empty() || f.content.back() == '\n' ? f.content : f.content + "\n";
        o << "        write_file(kernel, " << literal(f.path) << ", " << literal(content) << ");\n";
    }
    for (const ProgramSpec* p : programs)
        o << "        kernel.programs().add<" << class_name(p->name) << ">(" << literal(p->name) << ", "
          << literal(p->description.empty() ? (find_program_template(p->kind)->label) : p->description) << ");\n";
    std::set<std::pair<std::string, int>> seen_irq;
    for (const InterruptSpec& i : s.interrupts) {
        const bool vector = i.source == "vector";
        if (!(vector ? (i.number >= 48 && i.number <= 255) : (i.source == "irq" && i.number >= 0 && i.number <= 15)))
            continue;
        if (!seen_irq.insert({i.source, i.number}).second) continue;
        if (i.action == "program" && i.text != "sh" && !names.count(i.text)) continue;
        if (i.action != "program" && i.action != "message") continue;
        char num[16];
        std::snprintf(num, sizeof num, vector ? "0x%02x" : "%d", i.number);
        o << "        kernel.interrupts()." << (vector ? "on_vector(" : "on_irq(") << num << ", "
          << literal(i.name.empty() ? (vector ? std::string("vector ") + num : std::string("irq ") + num) : i.name)
          << ", [](osef::Kernel& k, uint8_t, uint32_t) {\n";
        if (i.action == "program")
            o << "            k.exec(" << literal(i.text) << ");\n";
        else
            o << "            k.print(" << literal(i.text.empty() || i.text.back() == '\n' ? i.text : i.text + "\n")
              << ");\n";
        o << "        });\n";
    }
    o << "    }\n";
    if (!s.files.empty())
        o << "\nprivate:\n"
             "    // Creates the parent folders, then the file.\n"
             "    static void write_file(osef::Kernel& kernel, osef::StringView path, osef::StringView content) {\n"
             "        osef::VFS& vfs = kernel.vfs();\n"
             "        for (size_t i = 1; i < path.size(); ++i)\n"
             "            if (path[i] == '/') vfs.mkdir(path.substr(0, i));\n"
             "        vfs.write(path, content);\n"
             "    }\n";
    o << "};\n\n"
         "} // namespace osef_generated\n\n"
         "OSEF_AUTO_MODULE(osef_generated::EditorSettings);\n";
    return o.str();
}

bool write_settings_files(const fs::path& dir, const OsSettings& settings, std::string* error) {
    return save_settings(dir / kSettingsFile, settings, error) &&
           write_if_changed(dir / kGeneratedFile, generate_settings_code(settings), error);
}

} // namespace osef::editor
