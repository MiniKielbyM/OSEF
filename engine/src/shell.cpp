// The built-in shell. Builtins write into a string buffer, so any builtin's
// output can be redirected with `>` or `>>`.

#include <osef/shell.h>
#include <osef/kernel.h>

namespace osef {
namespace {

using Args = Vector<String>;

struct Builtin {
    const char* name;
    const char* usage;
    int (*fn)(Context& c, const Args& args, String& out);
};

const Vector<Builtin>& builtins();

bool is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'; }

Vector<String> tokenize(StringView line) {
    Vector<String> out;
    String cur;
    bool in_token = false;
    char quote = 0;
    for (char ch : line) {
        if (quote) {
            if (ch == quote) quote = 0;
            else cur += ch;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            in_token = true;
        } else if (is_space(ch)) {
            if (in_token) out.push_back(cur);
            cur.clear();
            in_token = false;
        } else {
            cur += ch;
            in_token = true;
        }
    }
    if (in_token) out.push_back(cur);
    return out;
}

String join(const Args& args, size_t from = 0) {
    String out;
    for (size_t i = from; i < args.size(); ++i) {
        if (i > from) out += ' ';
        out += args[i];
    }
    return out;
}

String pad(StringView s, size_t width) { return s + String(s.size() < width ? width - s.size() : 1, ' '); }

int parse_int_or(StringView s, int fallback) {
    long long v = 0;
    if (!parse_int(s, v) || v < INT_MIN || v > INT_MAX) return fallback;
    return static_cast<int>(v);
}

const Builtin* find_builtin(StringView name) {
    for (const auto& b : builtins())
        if (name == b.name) return &b;
    return nullptr;
}

// "64 MiB", "1536 KiB"
String human_size(uint64_t bytes) {
    const unsigned long long kib = bytes / 1024;
    const unsigned long long mib = bytes / (1024 * 1024);
    return format("%10llu KiB  %6llu MiB", kib, mib);
}

String device_names(uint32_t devices) {
    String out;
    auto add = [&](uint32_t flag, const char* name) {
        if (!(devices & flag)) return;
        if (!out.empty()) out += ' ';
        out += name;
    };
    add(Hw::Serial, "serial");
    add(Hw::Framebuffer, "framebuffer");
    add(Hw::PS2Keyboard, "ps2-keyboard");
    add(Hw::RTC, "rtc");
    add(Hw::PCI, "pci");
    return out.empty() ? String("none") : out;
}

const Vector<Builtin>& builtins() {
    static const Vector<Builtin> table = {
        {"help", "list commands", [](Context& c, const Args&, String& out) {
             out += "builtins:\n";
             for (const auto& b : builtins()) out += "  " + pad(b.name, 10) + b.usage + "\n";
             out += "programs:\n";
             for (const auto& [name, prog] : c.kernel().programs().all())
                 out += "  " + pad(name, 10) + prog.description + "\n";
             out += "append '&' to run a program in the background, '> file' to redirect a builtin\n";
             return 0;
         }},
        {"echo", "echo <text...>", [](Context&, const Args& a, String& out) {
             out += join(a) + "\n";
             return 0;
         }},
        {"time", "show the local wall clock", [](Context& c, const Args&, String& out) {
             WallClock t{};
             if (!c.kernel().platform().read_wall_clock(t)) {
                 out += "time: clock unavailable (enable osef::Hw::RTC on bare metal)\n";
                 return 1;
             }
             out += format("%04u-%02u-%02u %02u:%02u:%02u (local time)\n", static_cast<unsigned>(t.year),
                           static_cast<unsigned>(t.month), static_cast<unsigned>(t.day),
                           static_cast<unsigned>(t.hour), static_cast<unsigned>(t.minute),
                           static_cast<unsigned>(t.second));
             return 0;
         }},
        {"lspci", "list PCI devices (bare metal; enable osef::Hw::PCI)", [](Context& c, const Args&, String& out) {
             Vector<PciDevice> devices;
             if (!c.kernel().platform().pci_devices(devices)) {
                 out += "lspci: PCI enumeration unavailable (enable osef::Hw::PCI on bare metal)\n";
                 return 1;
             }
             for (const PciDevice& d : devices)
                 out += format("%02x:%02x.%u  %04x:%04x  class %02x:%02x:%02x\n",
                               static_cast<unsigned>(d.bus), static_cast<unsigned>(d.device),
                               static_cast<unsigned>(d.function), static_cast<unsigned>(d.vendor_id),
                               static_cast<unsigned>(d.device_id), static_cast<unsigned>(d.class_code),
                               static_cast<unsigned>(d.subclass), static_cast<unsigned>(d.programming_interface));
             if (devices.empty()) out += "no PCI devices found\n";
             return 0;
         }},
        {"ls", "ls [path]", [](Context& c, const Args& a, String& out) {
             String target = c.resolve(a.empty() ? StringView(".") : StringView(a[0]));
             Vector<DirEntry> entries;
             if (!c.fs().list(target, entries)) {
                 out += "ls: cannot list " + target + "\n";
                 return 1;
             }
             for (const auto& e : entries) {
                 out += format("%-4s %8llu  ", to_string(e.type), static_cast<unsigned long long>(e.size));
                 out += e.name;
                 out += e.type == NodeType::Directory ? "/\n" : "\n";
             }
             return 0;
         }},
        {"cat", "cat <file...>", [](Context& c, const Args& a, String& out) {
             int rc = 0;
             for (const auto& f : a) {
                 String data;
                 if (c.read_file(f, data)) out += data;
                 else {
                     out += "cat: " + f + ": cannot read\n";
                     rc = 1;
                 }
             }
             return rc;
         }},
        {"cd", "cd [path]", [](Context& c, const Args& a, String& out) {
             StringView target = a.empty() ? StringView("/") : StringView(a[0]);
             if (c.chdir(target)) return 0;
             out += "cd: no such directory: " + target + "\n";
             return 1;
         }},
        {"pwd", "print working directory", [](Context& c, const Args&, String& out) {
             out += c.cwd() + "\n";
             return 0;
         }},
        {"mkdir", "mkdir <path>", [](Context& c, const Args& a, String& out) {
             if (!a.empty() && c.fs().mkdir(c.resolve(a[0]))) return 0;
             out += "mkdir: failed\n";
             return 1;
         }},
        {"touch", "touch <file>", [](Context& c, const Args& a, String& out) {
             if (!a.empty() && c.write_file(a[0], "", true)) return 0;
             out += "touch: failed\n";
             return 1;
         }},
        {"rm", "rm <path> (files and empty dirs)", [](Context& c, const Args& a, String& out) {
             if (!a.empty() && c.fs().remove(c.resolve(a[0]))) return 0;
             out += "rm: failed\n";
             return 1;
         }},
        {"mounts", "list mounted file systems", [](Context& c, const Args&, String& out) {
             for (const auto& [at, type] : c.fs().mounts()) out += at + "  (" + type + ")\n";
             return 0;
         }},
        {"ps", "list processes", [](Context& c, const Args&, String& out) {
             out += "  PID  PPID  PRI  STATE         STEPS  NAME\n";
             for (const auto& p : c.kernel().processes()) {
                 out += format("%5u %5u %4d  %-11s %7llu  ", p.pid, p.parent, p.priority, to_string(p.state),
                               static_cast<unsigned long long>(p.steps));
                 out += p.name;
                 out += p.pid == c.kernel().foreground() ? " *\n" : "\n";
             }
             return 0;
         }},
        {"kill", "kill <pid> [code]", [](Context& c, const Args& a, String& out) {
             if (a.empty()) {
                 out += "usage: kill <pid> [code]\n";
                 return 1;
             }
             int pid = parse_int_or(a[0], 0);
             if (pid > 0 && c.kernel().kill(static_cast<Pid>(pid), a.size() > 1 ? parse_int_or(a[1], -1) : -1)) return 0;
             out += "kill: no such process\n";
             return 1;
         }},
        {"send", "send <pid|service> <type> [text...]", [](Context& c, const Args& a, String& out) {
             if (a.size() < 2) {
                 out += "usage: send <pid|service> <type> [text...]\n";
                 return 1;
             }
             int pid = parse_int_or(a[0], 0);
             Pid to = pid > 0 ? static_cast<Pid>(pid) : c.lookup(a[0]);
             if (c.send(to, static_cast<uint32_t>(parse_int_or(a[1], 0)), join(a, 2))) return 0;
             out += "send: no such process or service\n";
             return 1;
         }},
        {"services", "list IPC services", [](Context& c, const Args&, String& out) {
             for (const auto& [name, pid] : c.kernel().ipc().services()) out += format("%s -> pid %u\n", name.c_str(), pid);
             return 0;
         }},
        {"sys", "sys <syscall> [args...]  (sys alone lists them)", [](Context& c, const Args& a, String& out) {
             if (a.empty()) {
                 for (const auto& [name, e] : c.kernel().syscalls().entries())
                     out += "  " + name + " - " + e.description + "\n";
                 return 0;
             }
             SyscallResult r = c.syscall(a[0], Args(a.begin() + 1, a.end()));
             out += format("%lld", static_cast<long long>(r.code));
             if (!r.data.empty()) out += "  " + r.data;
             out += '\n';
             return r.code < 0 ? 1 : 0;
         }},
        {"mods", "list loaded modules", [](Context& c, const Args&, String& out) {
             for (const auto& m : c.kernel().modules()) out += String(m->name()) + "\n";
             return 0;
         }},
        {"sched", "show scheduler", [](Context& c, const Args&, String& out) {
             out += String(c.kernel().scheduler().name()) + "\n";
             return 0;
         }},
        {"hw", "show the declared hardware (OSEF_HARDWARE)", [](Context& c, const Args&, String& out) {
             const HardwareSpec& hw = c.kernel().hardware();
             out += format("name:       %s\n", hw.name);
             out += format("arch:       %s\n", to_string(static_cast<Arch>(hw.arch)));
             out += format("firmware:   %s\n", to_string(static_cast<Firmware>(hw.firmware)));
             out += "devices:    " + device_names(hw.devices) + "\n";
             out += format("memory:     %u MiB\n", hw.memory_mb);
             if (hw.devices & Hw::Serial) out += format("serial:     %u baud\n", hw.serial_baud);
             else out += "serial:     not declared\n";
             if (hw.fb_width && hw.fb_height) out += format("resolution: %ux%u\n", hw.fb_width, hw.fb_height);
             else out += "resolution: firmware default\n";
             return 0;
         }},
        {"mem", "memory usage", [](Context& c, const Args&, String& out) {
             MemoryStats m = c.kernel().platform().memory();
             if (m.total == 0) {
                 out += "memory: n/a (not reported by this platform)\n";
                 return 0;
             }
             const uint64_t used = m.used < m.total ? m.used : m.total;
             out += "total " + human_size(m.total) + "\n";
             out += "used  " + human_size(used) + "\n";
             out += "free  " + human_size(m.total - used) + "\n";
             return 0;
         }},
        {"uptime", "time since boot", [](Context& c, const Args&, String& out) {
             const unsigned long long ms = c.kernel().uptime_ms();
             out += format("%llu.%llus, %llu ticks\n", ms / 1000, ms % 1000 / 100,
                           static_cast<unsigned long long>(c.kernel().ticks()));
             return 0;
         }},
        {"uname", "system information", [](Context& c, const Args&, String& out) {
             out += c.syscall("uname").data + "\n";
             return 0;
         }},
        {"clear", "clear the screen", [](Context&, const Args&, String& out) {
             out += "\x1b[2J\x1b[H";
             return 0;
         }},
        {"exit", "exit [code]  (exit this shell)", [](Context& c, const Args& a, String&) {
             c.exit(a.empty() ? 0 : parse_int_or(a[0], 0));
             return 0;
         }},
        {"shutdown", "shutdown [code]  (halt the system)", [](Context& c, const Args& a, String&) {
             c.kernel().shutdown(a.empty() ? 0 : parse_int_or(a[0], 0));
             return 0;
         }},
    };
    return table;
}

class Shell final : public Process {
public:
    void on_start(Context& c) override {
        String motd;
        if (c.fs().read("/etc/motd", motd)) c.print(motd);
    }

    void step(Context& c) override {
        if (show_prompt_) {
            c.print("\x1b[1;32m" + c.kernel().config().os_name + "\x1b[0m:\x1b[1;34m" + c.cwd() + "\x1b[0m$ ");
            show_prompt_ = false;
        }
        String line;
        if (!c.read_line(line)) {
            if (c.input_eof()) {
                c.println();
                c.exit(0);
            }
            return; // blocked until input arrives
        }
        if (!c.kernel().platform().interactive()) c.println(line); // echo scripted input
        show_prompt_ = true;
        execute(c, line);
    }

private:
    void execute(Context& c, StringView line) {
        Args toks = tokenize(line);
        if (toks.empty() || toks[0][0] == '#') return;

        bool background = false;
        if (toks.back() == "&") {
            background = true;
            toks.pop_back();
            if (toks.empty()) return;
        }

        String redirect;
        bool append = false;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i] != ">" && toks[i] != ">>") continue;
            if (i + 1 >= toks.size()) {
                c.println("sh: missing redirect target");
                return;
            }
            append = toks[i] == ">>";
            redirect = toks[i + 1];
            toks.erase(toks.begin() + i, toks.begin() + i + 2);
            break;
        }
        if (toks.empty()) return;

        const String name = toks[0];
        Args args(toks.begin() + 1, toks.end());

        if (const Builtin* b = find_builtin(name)) {
            String out;
            b->fn(c, args, out);
            if (redirect.empty()) c.print(out);
            else if (!c.write_file(redirect, out, append)) c.println("sh: cannot write " + redirect);
            return;
        }

        if (c.kernel().programs().find(name)) {
            if (!redirect.empty()) c.println("sh: redirection is only supported for builtins");
            Pid pid = c.spawn(name, args, !background);
            if (background) c.println(format("[%u] %s", pid, name.c_str()));
            else c.wait(pid); // step() resumes when the child exits
            return;
        }

        c.println("sh: command not found: " + name + " (try 'help')");
    }

    bool show_prompt_ = true;
};

} // namespace

void ShellModule::on_load(Kernel& kernel) { kernel.programs().add<Shell>("sh", "the OSEF shell"); }

} // namespace osef
