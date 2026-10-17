// OSEF Editor entry point: command line, GLFW + OpenGL 3.2 core + Dear ImGui
// (docking) bootstrap, the main loop and the headless screenshot mode.
//
//   osef-editor [<project dir or main.cpp>] [--root <osef checkout>]
//               [--screenshot <out.png>] [--size <W>x<H>] [--frames <N>]
//               [--select <component id>] [--open <file>[:<line>]]
//               [--set <component id>.<property id>=<value>]...
//               [--toggle <component id>=on|off]...

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "app/editor_app.h"
#include "app/theme.h"
#include "app/window_icon.h"
#include "model/component.h"
#include "model/document.h"
#include "model/project.h"
#include "util/png_writer.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#ifndef OSEF_EDITOR_FONTS_DIR
#define OSEF_EDITOR_FONTS_DIR ""
#endif
#ifndef OSEF_ROOT_DIR
#define OSEF_ROOT_DIR ""
#endif

namespace fs = std::filesystem;
using namespace osef::editor;

namespace {

constexpr float kUiFontSize = 14.0f;
constexpr const char* kHardwareHeader = "engine/include/osef/hardware.h";

// ---- Command line ---------------------------------------------------------------

struct Options {
    std::string project;     // positional: OS directory or .cpp file
    std::string root;        // --root
    std::string screenshot;  // non-empty = headless screenshot mode
    int width = 1600;
    int height = 900;
    int frames = 5;
    std::string select;
    std::string open;
    std::vector<std::string> sets;
    std::vector<std::string> toggles;
};

void print_usage() {
    std::printf(
        "usage: osef-editor [<project dir or main.cpp>] [--root <osef checkout>]\n"
        "                   [--screenshot <out.png>] [--size <W>x<H>] [--frames <N>]\n"
        "                   [--select <component id>] [--open <file>[:<line>]]\n"
        "                   [--set <component id>.<property id>=<value>]...\n"
        "                   [--toggle <component id>=on|off]...\n"
        "\n"
        "  <project>     an OS directory containing main.cpp, or a .cpp file\n"
        "                (default: examples/myos in the OSEF checkout)\n"
        "  --root        the OSEF checkout (default: searched upward from the project,\n"
        "                then from the executable, for %s)\n"
        "  --screenshot  render headlessly into an offscreen framebuffer, write a PNG and\n"
        "                exit; documents are never saved\n"
        "  --size        screenshot size in pixels (default 1600x900)\n"
        "  --frames      frames to render before capturing (default 5)\n"
        "  --select      select a component, e.g. hw.keyboard\n"
        "  --open        open a file in the code editor, optionally at a line\n"
        "  --set         set an Inspector property (rewrites the hardware declaration),\n"
        "                e.g. --set boot.firmware.firmware=BIOS --set hw.memory.memory_mb=256\n"
        "                Bool: true/false/on/off/1/0  Enum: option label  Resolution: WxH or auto\n"
        "  --toggle      enable or disable a component, e.g. --toggle hw.keyboard=off\n",
        kHardwareHeader);
}

bool parse_int(const char* s, int min, int max, int* out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) return false;
    *out = static_cast<int>(v);
    return true;
}

int arg_error(const std::string& message) {
    std::fprintf(stderr, "osef-editor: %s (see --help)\n", message.c_str());
    return 2;
}

// Returns 0 to continue, otherwise the process exit code (help = -1 -> 0).
int parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string inline_value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0) {
            const size_t eq = arg.find('=');
            // "--set a.b=c" keeps its '=' in the value; "--size=1x1" style is accepted too.
            if (eq != std::string::npos && arg.compare(0, 5, "--set") != 0 && arg.compare(0, 8, "--toggle") != 0) {
                inline_value = arg.substr(eq + 1);
                arg.resize(eq);
                has_inline = true;
            }
        }
        auto value = [&]() -> const char* {
            if (has_inline) return inline_value.c_str();
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return -1;
        } else if (arg == "--root") {
            const char* v = value();
            if (!v || !*v) return arg_error("--root needs a path");
            opt.root = v;
        } else if (arg == "--screenshot") {
            const char* v = value();
            if (!v || !*v) return arg_error("--screenshot needs a path");
            opt.screenshot = v;
        } else if (arg == "--size") {
            const char* v = value();
            const char* x = v ? std::strchr(v, 'x') : nullptr;
            if (!x) return arg_error("--size expects <W>x<H>");
            const std::string w(v, x);
            if (!parse_int(w.c_str(), 16, 16384, &opt.width) || !parse_int(x + 1, 16, 16384, &opt.height))
                return arg_error(std::string("bad --size '") + v + "'");
        } else if (arg == "--frames") {
            const char* v = value();
            if (!v || !parse_int(v, 1, 10000, &opt.frames)) return arg_error("--frames expects a positive integer");
        } else if (arg == "--select") {
            const char* v = value();
            if (!v || !*v) return arg_error("--select needs a component id");
            opt.select = v;
        } else if (arg == "--open") {
            const char* v = value();
            if (!v || !*v) return arg_error("--open needs a file");
            opt.open = v;
        } else if (arg == "--set") {
            const char* v = value();
            if (!v || !std::strchr(v, '=')) return arg_error("--set expects <component id>.<property id>=<value>");
            opt.sets.emplace_back(v);
        } else if (arg == "--toggle") {
            const char* v = value();
            if (!v || !std::strchr(v, '=')) return arg_error("--toggle expects <component id>=on|off");
            opt.toggles.emplace_back(v);
        } else if (!arg.empty() && arg[0] == '-') {
            return arg_error("unknown option '" + arg + "'");
        } else if (opt.project.empty()) {
            opt.project = arg;
        } else {
            return arg_error("more than one project given ('" + opt.project + "' and '" + arg + "')");
        }
    }
    return 0;
}

// ---- Paths ----------------------------------------------------------------------

fs::path executable_path(const char* argv0) {
    std::error_code ec;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
        if (!ec) return p;
    }
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) return fs::path(buf);
#else
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;
#endif
    fs::path p = fs::absolute(argv0 ? argv0 : "", ec);
    return ec ? fs::path() : p;
}

// First of: $OSEF_EDITOR_FONTS_DIR, <exe dir>/fonts, <exe dir>/../share/osef-editor/fonts,
// the build-time third_party/fonts. Empty if none has the UI font.
fs::path find_fonts_dir(const fs::path& exe) {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("OSEF_EDITOR_FONTS_DIR"); env && *env) candidates.emplace_back(env);
    if (!exe.empty()) {
        candidates.push_back(exe.parent_path() / "fonts");
        candidates.push_back(exe.parent_path().parent_path() / "share" / "osef-editor" / "fonts");
    }
    if (*OSEF_EDITOR_FONTS_DIR) candidates.emplace_back(OSEF_EDITOR_FONTS_DIR);
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(dir / "Inter-Regular.ttf", ec)) return dir;
    }
    return {};
}

// The nearest directory at or above `start` that contains engine/include/osef/hardware.h.
fs::path find_root_upward(const fs::path& start) {
    if (start.empty()) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(start, ec), ec);
    if (ec) return {};
    if (!fs::is_directory(p, ec)) p = p.parent_path();
    for (;;) {
        if (fs::is_regular_file(p / kHardwareHeader, ec)) return p;
        const fs::path parent = p.parent_path();
        if (parent.empty() || parent == p) return {};
        p = parent;
    }
}

fs::path detect_root(const Options& opt, const fs::path& exe) {
    std::error_code ec;
    if (!opt.root.empty()) return fs::weakly_canonical(fs::absolute(opt.root, ec), ec);
    if (fs::path r = find_root_upward(opt.project); !r.empty()) return r;
    if (fs::path r = find_root_upward(exe.parent_path()); !r.empty()) return r;
    if (*OSEF_ROOT_DIR && fs::is_regular_file(fs::path(OSEF_ROOT_DIR) / kHardwareHeader, ec)) return OSEF_ROOT_DIR;
    return find_root_upward(fs::current_path(ec));
}

// imgui.ini lives in the per-user config directory.
fs::path user_config_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) return fs::path(appdata) / "OSEF Editor";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / "Library" / "Application Support" / "OSEF Editor";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "osef-editor";
    if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / ".config" / "osef-editor";
#endif
    return {};
}

// ---- Command-line edits (--select/--open/--set/--toggle) -------------------------

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_bool(const std::string& v, bool* out) {
    const std::string s = lower(v);
    if (s == "true" || s == "on" || s == "1" || s == "yes") {
        *out = true;
        return true;
    }
    if (s == "false" || s == "off" || s == "0" || s == "no") {
        *out = false;
        return true;
    }
    return false;
}

bool parse_i64(const std::string& s, int64_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (*end != '\0') return false;
    *out = v;
    return true;
}

std::string property_ids(const Component& c) {
    std::string ids;
    for (const Property& p : c.properties) {
        if (p.read_only || p.type == PropertyType::Info) continue;
        ids += (ids.empty() ? "" : ", ") + p.id;
    }
    return ids.empty() ? "none" : ids;
}

// --set <component id>.<property id>=<value>, applied through Project::apply_property
// exactly like an Inspector edit.
bool apply_set(EditorContext& ctx, const std::string& spec, std::string* error) {
    const size_t eq = spec.find('=');
    const std::string key = spec.substr(0, eq);
    const std::string value = spec.substr(eq + 1);
    const size_t dot = key.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == key.size()) {
        *error = "--set " + spec + ": expected <component id>.<property id>=<value>";
        return false;
    }
    const std::string component_id = key.substr(0, dot);
    const std::string property_id = key.substr(dot + 1);
    Component* c = ctx.project->find(component_id);
    if (!c) {
        *error = "--set " + spec + ": unknown component '" + component_id + "'";
        return false;
    }
    Property* p = c->property(property_id);
    if (!p) {
        *error = "--set " + spec + ": " + component_id + " has no property '" + property_id +
                 "' (editable: " + property_ids(*c) + ")";
        return false;
    }
    if (p->read_only || p->type == PropertyType::Info) {
        *error = "--set " + spec + ": " + component_id + "." + property_id + " is read-only";
        return false;
    }
    switch (p->type) {
    case PropertyType::Bool:
        if (!parse_bool(value, &p->b)) {
            *error = "--set " + spec + ": expected true/false/on/off/1/0";
            return false;
        }
        break;
    case PropertyType::Int: {
        int64_t v = 0;
        if (!parse_i64(value, &v)) {
            *error = "--set " + spec + ": expected an integer";
            return false;
        }
        if (p->max > p->min && (v < p->min || v > p->max)) {
            *error = "--set " + spec + ": out of range [" + std::to_string(p->min) + ", " + std::to_string(p->max) +
                     "]";
            return false;
        }
        p->i = v;
        break;
    }
    case PropertyType::Enum: {
        const EnumOption* match = nullptr;
        for (const EnumOption& o : p->options)
            if (lower(o.label) == lower(value)) match = &o;
        int64_t numeric = 0;
        if (!match && parse_i64(value, &numeric))
            for (const EnumOption& o : p->options)
                if (o.value == numeric) match = &o;
        if (!match) {
            std::string labels;
            for (const EnumOption& o : p->options) labels += (labels.empty() ? "" : ", ") + o.label;
            *error = "--set " + spec + ": expected one of " + labels;
            return false;
        }
        p->i = match->value;
        break;
    }
    case PropertyType::String: p->s = value; break;
    case PropertyType::Resolution: {
        const std::string v = lower(value);
        if (v == "auto" || v == "0" || v == "0x0") {
            p->i = p->i2 = 0;
            break;
        }
        const size_t x = v.find('x');
        int64_t w = 0, h = 0;
        if (x == std::string::npos || !parse_i64(v.substr(0, x), &w) || !parse_i64(v.substr(x + 1), &h) || w < 0 ||
            h < 0) {
            *error = "--set " + spec + ": expected <W>x<H> or auto";
            return false;
        }
        p->i = w;
        p->i2 = h;
        break;
    }
    case PropertyType::Info: break;
    }
    ctx.project->apply_property(*c, *p);
    ctx.project->sync_from_code();
    return true;
}

bool apply_toggle(EditorContext& ctx, const std::string& spec, std::string* error) {
    const size_t eq = spec.find('=');
    const std::string id = spec.substr(0, eq);
    bool on = false;
    if (!parse_bool(spec.substr(eq + 1), &on)) {
        *error = "--toggle " + spec + ": expected on or off";
        return false;
    }
    Component* c = ctx.project->find(id);
    if (!c) {
        *error = "--toggle " + spec + ": unknown component '" + id + "'";
        return false;
    }
    if (!c->toggleable) {
        *error = "--toggle " + spec + ": " + id + " cannot be enabled or disabled";
        return false;
    }
    c->enabled = on;
    ctx.project->apply_enabled(*c);
    ctx.project->sync_from_code();
    return true;
}

// "--open file[:line]": relative paths are tried against the current
// directory, the project and the OSEF checkout.
bool apply_open(EditorContext& ctx, const std::string& spec, std::string* error) {
    std::string file = spec;
    int line = 0;
    const size_t colon = spec.rfind(':');
    if (colon != std::string::npos && colon + 1 < spec.size() &&
        std::all_of(spec.begin() + static_cast<std::ptrdiff_t>(colon) + 1, spec.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        file = spec.substr(0, colon);
        line = std::atoi(spec.c_str() + colon + 1);
    }
    std::vector<fs::path> tries;
    const fs::path p(file);
    std::error_code ec;
    if (p.is_absolute()) {
        tries.push_back(p);
    } else {
        tries.push_back(fs::absolute(p, ec));
        if (ctx.project) tries.push_back(ctx.project->directory() / p);
        if (!ctx.engine_root.empty()) tries.push_back(ctx.engine_root / p);
    }
    for (const fs::path& t : tries) {
        if (fs::is_regular_file(t, ec)) {
            if (!ctx.open_file(t, line)) {
                *error = "--open " + spec + ": cannot open " + t.string();
                return false;
            }
            return true;
        }
    }
    *error = "--open " + spec + ": file not found";
    return false;
}

// Returns false if any command-line edit failed (each failure is reported).
bool apply_cli_actions(EditorContext& ctx, const Options& opt) {
    bool ok = true;
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "osef-editor: %s\n", message.c_str());
        ctx.log.error(message);
        ok = false;
    };
    const bool edits = !opt.sets.empty() || !opt.toggles.empty() || !opt.select.empty();
    if (!ctx.project) {
        if (edits) fail("--set/--toggle/--select need a project");
    } else {
        for (const std::string& s : opt.sets) {
            std::string error;
            if (apply_set(ctx, s, &error))
                ctx.log.info("--set " + s);
            else
                fail(error);
        }
        for (const std::string& t : opt.toggles) {
            std::string error;
            if (apply_toggle(ctx, t, &error))
                ctx.log.info("--toggle " + t);
            else
                fail(error);
        }
        if (!opt.select.empty()) {
            if (ctx.project->find(opt.select))
                ctx.select(opt.select);
            else
                fail("--select " + opt.select + ": unknown component");
        }
    }
    if (!opt.open.empty()) {
        std::string error;
        if (!apply_open(ctx, opt.open, &error)) fail(error);
    }
    return ok;
}

// ---- GL ------------------------------------------------------------------------
// The ImGui backend loads its own GL functions; this covers what the
// offscreen target needs, loaded through GLFW so it works on every platform.

#if defined(_WIN32)
#define OSEF_GLAPI __stdcall
#else
#define OSEF_GLAPI
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLfloat = float;

constexpr GLenum kGL_FRAMEBUFFER = 0x8D40;
constexpr GLenum kGL_RENDERBUFFER = 0x8D41;
constexpr GLenum kGL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum kGL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum kGL_RGBA8 = 0x8058;
constexpr GLenum kGL_RGBA = 0x1908;
constexpr GLenum kGL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum kGL_PACK_ALIGNMENT = 0x0D05;
constexpr GLbitfield kGL_COLOR_BUFFER_BIT = 0x00004000;

struct Gl {
    void(OSEF_GLAPI* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void(OSEF_GLAPI* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void(OSEF_GLAPI* BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum(OSEF_GLAPI* CheckFramebufferStatus)(GLenum) = nullptr;
    void(OSEF_GLAPI* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void(OSEF_GLAPI* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void(OSEF_GLAPI* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void(OSEF_GLAPI* BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void(OSEF_GLAPI* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void(OSEF_GLAPI* Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(OSEF_GLAPI* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(OSEF_GLAPI* Clear)(GLbitfield) = nullptr;
    void(OSEF_GLAPI* PixelStorei)(GLenum, GLint) = nullptr;
    void(OSEF_GLAPI* ReadBuffer)(GLenum) = nullptr;
    void(OSEF_GLAPI* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(OSEF_GLAPI* Finish)() = nullptr;

    bool load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(glfwGetProcAddress(name));
            if (!fn) {
                std::fprintf(stderr, "osef-editor: missing GL function %s\n", name);
                ok = false;
            }
        };
        get(GenFramebuffers, "glGenFramebuffers");
        get(DeleteFramebuffers, "glDeleteFramebuffers");
        get(BindFramebuffer, "glBindFramebuffer");
        get(CheckFramebufferStatus, "glCheckFramebufferStatus");
        get(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
        get(GenRenderbuffers, "glGenRenderbuffers");
        get(DeleteRenderbuffers, "glDeleteRenderbuffers");
        get(BindRenderbuffer, "glBindRenderbuffer");
        get(RenderbufferStorage, "glRenderbufferStorage");
        get(Viewport, "glViewport");
        get(ClearColor, "glClearColor");
        get(Clear, "glClear");
        get(PixelStorei, "glPixelStorei");
        get(ReadBuffer, "glReadBuffer");
        get(ReadPixels, "glReadPixels");
        get(Finish, "glFinish");
        return ok;
    }
};

Gl gl;

// An RGBA8 framebuffer object of exactly width x height pixels.
class OffscreenTarget {
public:
    bool create(int width, int height) {
        width_ = width;
        height_ = height;
        gl.GenFramebuffers(1, &fbo_);
        gl.GenRenderbuffers(1, &color_);
        gl.BindRenderbuffer(kGL_RENDERBUFFER, color_);
        gl.RenderbufferStorage(kGL_RENDERBUFFER, kGL_RGBA8, width, height);
        gl.BindFramebuffer(kGL_FRAMEBUFFER, fbo_);
        gl.FramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_RENDERBUFFER, color_);
        const bool complete = gl.CheckFramebufferStatus(kGL_FRAMEBUFFER) == kGL_FRAMEBUFFER_COMPLETE;
        gl.BindFramebuffer(kGL_FRAMEBUFFER, 0);
        return complete;
    }
    void bind() const { gl.BindFramebuffer(kGL_FRAMEBUFFER, fbo_); }

    // Rows come back bottom-up (OpenGL convention); alpha is forced opaque.
    std::vector<uint8_t> read_rgba() const {
        std::vector<uint8_t> pixels(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
        bind();
        gl.Finish();
        gl.ReadBuffer(kGL_COLOR_ATTACHMENT0);
        gl.PixelStorei(kGL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, width_, height_, kGL_RGBA, kGL_UNSIGNED_BYTE, pixels.data());
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        return pixels;
    }
    void destroy() {
        if (fbo_) gl.DeleteFramebuffers(1, &fbo_);
        if (color_) gl.DeleteRenderbuffers(1, &color_);
        fbo_ = color_ = 0;
    }

private:
    GLuint fbo_ = 0, color_ = 0;
    int width_ = 0, height_ = 0;
};

// ---- Main loop --------------------------------------------------------------------

// SIGINT/SIGTERM ask to quit (with the unsaved-changes prompt); a second one forces it.
volatile std::sig_atomic_t g_quit_signals = 0;

void on_quit_signal(int) { g_quit_signals = g_quit_signals + 1; }

void glfw_error(int code, const char* description) {
    std::fprintf(stderr, "osef-editor: GLFW error %d: %s\n", code, description);
}

float window_content_scale(GLFWwindow* window) {
#if defined(__APPLE__)
    // macOS reports sizes in points and ImGui rasterizes fonts at the
    // framebuffer scale, so nothing extra to scale.
    (void)window;
    return 1.0f;
#else
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window, &sx, &sy);
    return (sx > 0.5f && sx < 4.0f) ? sx : 1.0f;
#endif
}

int run(const Options& opt, const fs::path& exe) {
    const bool headless = !opt.screenshot.empty();
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // Resolve the checkout and the project before opening a window.
    const fs::path root = detect_root(opt, exe);
    std::error_code ec;
    if (!root.empty() && !fs::is_regular_file(root / kHardwareHeader, ec))
        std::fprintf(stderr, "osef-editor: warning: %s has no %s\n", root.string().c_str(), kHardwareHeader);
    fs::path project_path;
    if (!opt.project.empty())
        project_path = fs::weakly_canonical(fs::absolute(opt.project, ec), ec);
    else if (!root.empty())
        project_path = root / "examples" / "myos";

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;

    // OpenGL 3.2 core; macOS requires forward-compatible core profiles.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);  // Retina framebuffer
#else
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // 1600x900 means "at 100%" on HiDPI monitors
#endif
    if (headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    const int win_w = headless ? opt.width : 1600;
    const int win_h = headless ? opt.height : 900;
    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "OSEF Editor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    if (!headless) set_window_icon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(headless ? 0 : 1);  // vsync when windowed
    if (!gl.load()) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!headless) {
        // Bigger than the screen's work area (small laptop displays): maximize instead.
        int mx = 0, my = 0, mw = 0, mh = 0, ww = 0, wh = 0;
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        glfwGetWindowSize(window, &ww, &wh);
        if (mw > 0 && mh > 0 && (ww >= mw || wh >= mh)) glfwMaximizeWindow(window);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Persistent layout in the user config dir (never in screenshot mode).
    static std::string ini_path;
    io.IniFilename = nullptr;
    if (!headless) {
        const fs::path dir = user_config_dir();
        if (!dir.empty()) fs::create_directories(dir, ec);
        if (!dir.empty() && fs::is_directory(dir, ec)) {
            ini_path = (dir / "imgui.ini").string();
            io.IniFilename = ini_path.c_str();
        }
    }

    {
        EditorApp app;
        EditorContext& ctx = app.context();
        app.set_screenshot_mode(headless);
        app.install_settings_handler();

        float ui_scale = headless ? 1.0f : window_content_scale(window);
        theme::apply_style(ui_scale);
        const fs::path fonts_dir = find_fonts_dir(exe);
        theme::load_fonts(fonts_dir, kUiFontSize, ctx.fonts);
        if (fonts_dir.empty()) {
            std::fprintf(stderr, "osef-editor: fonts not found, using the built-in font "
                                 "(run tools/bootstrap/editor-deps.sh)\n");
            ctx.log.warn("Inter / JetBrains Mono not found: using the built-in font");
        }

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 150");

        // Project + command-line edits (they go through the same model APIs as the UI).
        bool cli_ok = true;
        ctx.engine_root = root;
        if (root.empty()) {
            const std::string msg = "Cannot find an OSEF checkout (looked for " + std::string(kHardwareHeader) +
                                    "); pass --root <dir>";
            std::fprintf(stderr, "osef-editor: %s\n", msg.c_str());
            ctx.log.error(msg);
            cli_ok = false;
        }
        if (!project_path.empty()) {
            std::string error;
            if (app.open_project(project_path, root, &error)) {
                std::printf("osef-editor: project %s (%s), engine %s\n", ctx.project->name().c_str(),
                            ctx.project->main_file().string().c_str(), root.string().c_str());
            } else {
                std::fprintf(stderr, "osef-editor: cannot open project %s: %s\n", project_path.string().c_str(),
                             error.c_str());
                cli_ok = false;
            }
        }
        if (!apply_cli_actions(ctx, opt)) cli_ok = false;

        int exit_code = 0;
        OffscreenTarget target;
        if (headless && !target.create(opt.width, opt.height)) {
            std::fprintf(stderr, "osef-editor: cannot create a %dx%d offscreen framebuffer\n", opt.width, opt.height);
            exit_code = 1;
        }

        std::string title;
        int handled_signals = 0;
        double active_until = glfwGetTime() + 1.0;  // render continuously until then (startup, recent input)
        const int frame_limit = headless ? opt.frames : -1;
        for (int frame = 0; exit_code == 0 && (frame_limit < 0 || frame < frame_limit); ++frame) {
            if (!headless) {
                if (glfwWindowShouldClose(window)) {
                    glfwSetWindowShouldClose(window, GLFW_FALSE);
                    app.request_quit();
                }
                if (g_quit_signals != handled_signals) {
                    handled_signals = g_quit_signals;
                    app.request_quit(/*force=*/handled_signals >= 2);
                }
                if (app.should_quit()) break;

                // Idle: sleep until input (or a 0.25 s tick for blinking cursors);
                // busy (recent input, running job): render every vsync.
                const double now = glfwGetTime();
                if (ctx.runner.running() || now < active_until) {
                    glfwPollEvents();
                } else {
                    glfwWaitEventsTimeout(0.25);
                    if (glfwGetTime() - now < 0.24) active_until = glfwGetTime() + 1.0;
                }
                if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                    glfwWaitEventsTimeout(0.1);
                    continue;
                }
#if !defined(__APPLE__)
                if (const float s = window_content_scale(window); s != ui_scale) {
                    ui_scale = s;  // moved to a monitor with another DPI
                    theme::apply_style(ui_scale);
                }
#endif
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (headless) {
                // Exactly W x H pixels regardless of the (hidden) window or Retina scale.
                io.DisplaySize = ImVec2(static_cast<float>(opt.width), static_cast<float>(opt.height));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                io.DeltaTime = 1.0f / 60.0f;
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            if (io.WantTextInput || ImGui::IsAnyMouseDown()) active_until = glfwGetTime() + 1.0;

            if (!headless) {
                if (std::string t = app.window_title(); t != title) {
                    title = std::move(t);
                    glfwSetWindowTitle(window, title.c_str());
                }
            }

            int fb_w = opt.width, fb_h = opt.height;
            if (headless)
                target.bind();
            else
                glfwGetFramebufferSize(window, &fb_w, &fb_h);
            gl.Viewport(0, 0, fb_w, fb_h);
            gl.ClearColor(0.098f, 0.098f, 0.098f, 1.0f);
            gl.Clear(kGL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (!headless) glfwSwapBuffers(window);
        }

        if (headless && exit_code == 0) {
            const std::vector<uint8_t> pixels = target.read_rgba();
            std::string error;
            if (write_png(opt.screenshot, opt.width, opt.height, pixels.data(), /*flip_vertically=*/true, &error)) {
                std::printf("osef-editor: wrote %s (%dx%d)\n", opt.screenshot.c_str(), opt.width, opt.height);
            } else {
                std::fprintf(stderr, "osef-editor: %s\n", error.c_str());
                exit_code = 1;
            }
            if (exit_code == 0 && !cli_ok) exit_code = 1;
        }
        target.destroy();

        ctx.runner.cancel();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        // imgui.ini is written by DestroyContext, while the app (and its
        // settings handler) still exists.
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return exit_code;
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (const int rc = parse_args(argc, argv, opt); rc != 0) return rc < 0 ? 0 : rc;
    return run(opt, executable_path(argc > 0 ? argv[0] : nullptr));
}
