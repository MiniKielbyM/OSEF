// OS Settings panel: plain-language forms over Project::settings() and the
// hardware declaration, with a live preview of the OS console.

#include "panels/settings_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"
#include "model/document.h"
#include "model/new_project.h"
#include "model/os_settings.h"
#include "model/project.h"

#include "imgui.h"
#include "imgui_internal.h"  // MarkIniSettingsDirty
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace osef::editor {

using theme::Color;

struct SettingsPanel::State {
    int add_template = 0;
};

namespace {

// The console palette (same as the bare-metal framebuffer console).
constexpr ImU32 kConsole[16] = {
    IM_COL32(0, 0, 0, 255),       IM_COL32(170, 0, 0, 255),     IM_COL32(0, 170, 0, 255),
    IM_COL32(170, 85, 0, 255),    IM_COL32(0, 0, 170, 255),     IM_COL32(170, 0, 170, 255),
    IM_COL32(0, 170, 170, 255),   IM_COL32(170, 170, 170, 255), IM_COL32(85, 85, 85, 255),
    IM_COL32(255, 85, 85, 255),   IM_COL32(85, 255, 85, 255),   IM_COL32(255, 255, 85, 255),
    IM_COL32(85, 85, 255, 255),   IM_COL32(255, 85, 255, 255),  IM_COL32(85, 255, 255, 255),
    IM_COL32(255, 255, 255, 255),
};

// color_names()[0] is "default"; the rest map to console colors 0-15.
ImU32 console_rgb(const std::string& name, int fallback) {
    const auto& names = color_names();
    for (size_t i = 1; i < names.size(); ++i)
        if (names[i] == name) return kConsole[i - 1];
    return kConsole[fallback];
}

std::string pretty_color(const std::string& name) {
    if (name == "default") return "Default";
    std::string out;
    bool upper = true;
    for (char c : name) {
        if (c == '-') {
            out += ' ';
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    return out;
}

float label_width() { return std::floor(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.36f)); }

// Label on the left, the next widget fills the rest of the row.
void row(const char* label, const char* help = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", help);
    ImGui::SameLine(label_width());
    ImGui::SetNextItemWidth(-1.0f);
}

void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color(Color::TextDim));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool section(EditorContext& ctx, const char* title, const char* subtitle) {
    ImGui::Spacing();
    if (ctx.fonts.ui_bold) ImGui::PushFont(ctx.fonts.ui_bold, 0.0f);
    const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
    if (ctx.fonts.ui_bold) ImGui::PopFont();
    if (open && subtitle) hint(subtitle);
    return open;
}

void swatch(ImU32 color) {
    const float s = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + s, p.y + s), color, 2.0f);
    dl->AddRect(ImVec2(p.x, p.y + 1), ImVec2(p.x + s, p.y + s), theme::color(Color::Border), 2.0f);
    ImGui::Dummy(ImVec2(s, s));
}

bool color_combo(const char* id, std::string& value, int default_index) {
    bool changed = false;
    const std::string preview = pretty_color(value);
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (const std::string& name : color_names()) {
            ImGui::PushID(name.c_str());
            swatch(console_rgb(name, default_index));
            ImGui::SameLine();
            if (ImGui::Selectable(pretty_color(name).c_str(), name == value)) {
                value = name;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A fake screen showing the OS console with the current appearance.
void preview(EditorContext& ctx, const OsSettings& s, const std::string& os_name) {
    const ImU32 bg = console_rgb(s.background, 0);
    const ImU32 fg = console_rgb(s.text_color, 7);
    const ImU32 name_c = s.prompt_name_color == "default" ? fg : console_rgb(s.prompt_name_color, 7);
    const ImU32 path_c = s.prompt_path_color == "default" ? fg : console_rgb(s.prompt_path_color, 7);
    ImFont* mono = ctx.fonts.mono ? ctx.fonts.mono : ImGui::GetFont();
    const float base = ImGui::GetFontSize() * 0.9f;
    const float size = std::floor(base * (s.text_size >= 2 ? 1.0f + 0.25f * float(s.text_size - 1) : 1.0f));
    const float line = std::floor(size * 1.25f);
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = line * 6.0f + 16.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 4.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), theme::color(Color::Border), 4.0f);
    dl->PushClipRect(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + w - 4, p.y + h - 4), true);
    float x = p.x + 10.0f, y = p.y + 8.0f;
    auto text = [&](ImU32 color, const std::string& t) {
        dl->AddText(mono, size, ImVec2(x, y), color, t.c_str());
        x += mono->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x;
    };
    auto newline = [&] {
        x = p.x + 10.0f;
        y += line;
    };
    text(fg, os_name + " 0.1.0  (OSEF engine)");
    newline();
    std::string welcome = s.welcome;
    size_t start = 0;
    int lines = 0;
    while (start <= welcome.size() && lines < 2) {
        const size_t nl = welcome.find('\n', start);
        const std::string part = welcome.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!part.empty() || nl != std::string::npos) {
            text(fg, part);
            newline();
            ++lines;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    // The prompt, with {os} and {cwd} filled in.
    for (int i = 0; i < 2; ++i) {
        const std::string& t = s.prompt;
        for (size_t k = 0; k < t.size();) {
            if (t.compare(k, 4, "{os}") == 0) {
                text(name_c, os_name);
                k += 4;
            } else if (t.compare(k, 5, "{cwd}") == 0) {
                text(path_c, i == 0 ? "/" : "/home");
                k += 5;
            } else {
                size_t next = std::min(t.find("{os}", k), t.find("{cwd}", k));
                if (next == k) next = k + 1;
                text(fg, t.substr(k, next == std::string::npos ? std::string::npos : next - k));
                k = next == std::string::npos ? t.size() : next;
            }
        }
        if (i == 0) {
            text(fg, "cd /home");
            newline();
        } else {
            dl->AddRectFilled(ImVec2(x, y + size * 0.85f), ImVec2(x + size * 0.55f, y + size), fg);
        }
    }
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h));
}

std::string unique_name(const OsSettings& s, const std::string& base) {
    auto taken = [&](const std::string& n) {
        if (std::find(reserved_command_names().begin(), reserved_command_names().end(), n) !=
            reserved_command_names().end())
            return true;
        return std::any_of(s.programs.begin(), s.programs.end(), [&](const ProgramSpec& p) { return p.name == n; });
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i)
        if (!taken(base + std::to_string(i))) return base + std::to_string(i);
}

std::string unique_file(const OsSettings& s) {
    for (int i = 1;; ++i) {
        const std::string path = i == 1 ? "/home/new.txt" : "/home/new" + std::to_string(i) + ".txt";
        if (std::none_of(s.files.begin(), s.files.end(), [&](const FileSpec& f) { return f.path == path; }))
            return path;
    }
}

bool big_button(const char* id, icons::Glyph glyph, ImU32 icon_color, const char* label, float width, bool enabled,
                const char* tooltip) {
    const float h = std::floor(ImGui::GetFrameHeight() * 1.5f);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(id, ImVec2(width, h));
    const ImVec2 p = ImGui::GetItemRectMin();
    const float icon = std::floor(ImGui::GetFontSize());
    const float tw = ImGui::CalcTextSize(label).x;
    const float x = p.x + std::floor((width - icon - 8.0f - tw) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    icons::draw(dl, ImVec2(x, std::floor(p.y + (h - icon) * 0.5f)), icon, glyph, icon_color);
    dl->AddText(ImVec2(x + icon + 8.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), theme::color(Color::Text), label);
    ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// Saves main.cpp after a hardware edit made here (the forms promise instant saving).
void save_main(EditorContext& ctx) {
    Document* doc = ctx.project ? ctx.project->main_document() : nullptr;
    if (!doc || !doc->modified()) return;
    std::string error;
    if (!doc->save(&error)) ctx.log.error("Cannot save " + doc->title() + ": " + error);
}

// ---------------------------------------------------------------- sections

void startup_section(EditorContext& ctx) {
    Project& project = *ctx.project;
    if (!section(ctx, "Start-up", "The basics: what your OS is called and what kind of PC it starts on.")) return;
    bool hw_changed = false;
    Component& os = project.root();
    if (Property* name = os.property("name")) {
        row("Name", "Shown when the OS starts and in its prompt (up to 31 characters).");
        std::string value = name->s;
        if (ImGui::InputText("##os_name", &value, ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            if (!value.empty() && value != name->s) {
                name->s = value.substr(0, 31);
                project.apply_property(os, *name);
                hw_changed = true;
            }
        }
    }
    if (Component* fw = project.find("boot.firmware")) {
        if (Property* p = fw->property("firmware")) {
            row("Type of PC", "How the computer starts the OS. The bootable ISO only works on this kind of PC.");
            for (const EnumOption& o : p->options) {
                const bool uefi = o.label == "UEFI";
                const std::string label = uefi ? "Modern PC (UEFI)" : "Older PC (BIOS)";
                if (ImGui::RadioButton(label.c_str(), p->i == o.value) && p->i != o.value) {
                    p->i = o.value;
                    project.apply_property(*fw, *p);
                    hw_changed = true;
                }
                ImGui::SetItemTooltip("%s", uefi ? "PCs from about 2012 on, and most virtual machines."
                                                 : "Older PCs, or new ones with \"legacy boot\" turned on.");
                if (&o != &p->options.back()) ImGui::SameLine();
            }
        }
    }
    if (Component* mem = project.find("hw.memory")) {
        if (Property* p = mem->property("memory_mb")) {
            row("Memory", "The least memory (RAM) the OS needs. The virtual PC gets this much.");
            int mb = static_cast<int>(p->i);
            if (ImGui::SliderInt("##memory", &mb, 32, 4096, "%d MiB", ImGuiSliderFlags_Logarithmic) ||
                ImGui::IsItemDeactivatedAfterEdit()) {
                mb = std::max(32, (mb + 16) / 32 * 32);
                if (mb != p->i) {
                    p->i = mb;
                    project.apply_property(*mem, *p);
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) hw_changed = true;
        }
    }
    if (hw_changed) save_main(ctx);
}

bool look_section(EditorContext& ctx, OsSettings& s) {
    if (!section(ctx, "Look & feel", "How your OS looks when it runs.")) return false;
    bool changed = false;
    preview(ctx, s, ctx.project->name());
    ImGui::Spacing();

    row("Style", "Ready-made color sets. You can still change each color below.");
    const LookPreset* look = current_look(s);
    if (ImGui::BeginCombo("##look", look ? look->name.c_str() : "Custom")) {
        for (const LookPreset& p : look_presets()) {
            ImGui::PushID(p.name.c_str());
            swatch(console_rgb(p.background, 0));
            ImGui::SameLine(0, 2);
            swatch(console_rgb(p.text_color, 7));
            ImGui::SameLine();
            if (ImGui::Selectable(p.name.c_str(), look == &p)) {
                apply_look(s, p);
                changed = true;
            }
            ImGui::SetItemTooltip("%s", p.description.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    row("Welcome message", "Shown when the OS starts. Press Enter for a new line.");
    changed |= ImGui::InputTextMultiline("##welcome", &s.welcome, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.2f));
    row("Prompt", "What the OS shows when it waits for a command. {os} is the OS name, {cwd} the current folder.");
    changed |= ImGui::InputText("##prompt", &s.prompt);
    row("Text color");
    changed |= color_combo("##text", s.text_color, 7);
    row("Background");
    changed |= color_combo("##bg", s.background, 0);
    row("Name color", "Color of the OS name in the prompt.");
    changed |= color_combo("##namec", s.prompt_name_color, 7);
    row("Folder color", "Color of the current folder in the prompt.");
    changed |= color_combo("##pathc", s.prompt_path_color, 7);
    row("Text size", "Bigger text is easier to read on large screens.");
    static const char* const sizes[] = {"Automatic", "Small (1x)", "Medium (2x)", "Large (3x)", "Huge (4x)"};
    changed |= ImGui::Combo("##size", &s.text_size, sizes, 5);
    return changed;
}

void hardware_section(EditorContext& ctx) {
    if (!section(ctx, "Hardware", "The devices your OS uses. At least the screen or the serial port must be on."))
        return;
    struct Dev {
        const char* id;
        const char* label;
        const char* help;
    };
    static const Dev devices[] = {
        {"hw.display", "Screen", "Shows text on the monitor (framebuffer console)."},
        {"hw.keyboard", "Keyboard", "Lets people type (PS/2 keyboard; USB keyboards usually work too)."},
        {"hw.serial", "Serial port", "Text over a serial cable. Handy for debugging and for the virtual PC's terminal."},
        {"hw.rtc", "Clock", "The real date and time (the PC's battery-backed clock)."},
        {"hw.pci", "Device list (PCI)", "Finds the PC's expansion devices; try the 'lspci' command."},
    };
    bool changed = false;
    for (const Dev& d : devices) {
        Component* c = ctx.project->find(d.id);
        if (!c || !c->toggleable) continue;
        bool on = c->enabled;
        if (ImGui::Checkbox(d.label, &on)) {
            c->enabled = on;
            ctx.project->apply_enabled(*c);
            changed = true;
        }
        ImGui::SameLine(label_width());
        hint(d.help);
    }
    ImGui::BeginDisabled();
    for (const char* planned : {"Mouse", "USB", "Disk storage", "Network", "Sound"}) {
        bool off = false;
        ImGui::Checkbox(planned, &off);
        ImGui::SameLine(label_width());
        hint("Coming later: OSEF does not support this device yet.");
    }
    ImGui::EndDisabled();
    if (changed) save_main(ctx);
}

bool programs_section(EditorContext& ctx, SettingsPanel::State& st, OsSettings& s) {
    if (!section(ctx, "Programs", "Commands people can run in your OS. Pick a type, give it a name, done."))
        return false;
    bool changed = false;
    changed |= ImGui::Checkbox("Include the command shell", &s.shell);
    ImGui::SetItemTooltip("The shell is where people type commands like 'help' or 'ls'.");

    row("Start at boot", "The first program the OS runs. When it finishes, the OS turns off.");
    const std::string current = s.start_program == "sh" ? "Command shell" : s.start_program;
    if (ImGui::BeginCombo("##start", current.c_str())) {
        if (ImGui::Selectable("Command shell", s.start_program == "sh")) {
            s.start_program = "sh";
            s.shell = true;
            changed = true;
        }
        for (const ProgramSpec& p : s.programs)
            if (ImGui::Selectable(p.name.c_str(), s.start_program == p.name)) {
                s.start_program = p.name;
                changed = true;
            }
        ImGui::EndCombo();
    }

    int remove = -1;
    for (size_t i = 0; i < s.programs.size(); ++i) {
        ProgramSpec& p = s.programs[i];
        const ProgramTemplate* t = find_program_template(p.kind);
        ImGui::PushID(static_cast<int>(i));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::PanelBg));
        ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        if (ctx.fonts.ui_bold) ImGui::PushFont(ctx.fonts.ui_bold, 0.0f);
        ImGui::TextUnformatted(t ? t->label.c_str() : p.kind.c_str());
        if (ctx.fonts.ui_bold) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        if (t) hint(t->description.c_str());
        row("Command", "What people type to run it: lowercase letters, numbers, - and _.");
        const std::string before = p.name;
        if (ImGui::InputText("##name", &p.name)) {
            if (s.start_program == before) s.start_program = p.name;
            changed = true;
        }
        if (!is_valid_command_name(p.name))
            ImGui::TextColored(theme::color_vec(Color::Warning), "Use lowercase letters, numbers, - or _ (start with a letter).");
        row("Description", "Shown by the 'help' command.");
        changed |= ImGui::InputText("##desc", &p.description);
        if (t && !t->text_label.empty()) {
            row(t->text_label.c_str());
            changed |= ImGui::InputText("##text", &p.text);
        }
        if (t && !t->number_label.empty()) {
            row(t->number_label.c_str());
            if (ImGui::InputInt("##num", &p.number)) {
                p.number = std::clamp(p.number, t->number_min, t->number_max);
                changed = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (remove >= 0) {
        if (s.start_program == s.programs[static_cast<size_t>(remove)].name) s.start_program = "sh";
        s.programs.erase(s.programs.begin() + remove);
        changed = true;
    }

    ImGui::Spacing();
    const auto& templates = program_templates();
    st.add_template = std::clamp(st.add_template, 0, static_cast<int>(templates.size()) - 1);
    ImGui::SetNextItemWidth(label_width() + 60.0f);
    if (ImGui::BeginCombo("##addkind", templates[static_cast<size_t>(st.add_template)].label.c_str())) {
        for (size_t i = 0; i < templates.size(); ++i) {
            if (ImGui::Selectable(templates[i].label.c_str(), static_cast<int>(i) == st.add_template))
                st.add_template = static_cast<int>(i);
            ImGui::SetItemTooltip("%s", templates[i].description.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add program")) {
        const ProgramTemplate& t = templates[static_cast<size_t>(st.add_template)];
        ProgramSpec p;
        p.kind = t.kind;
        p.name = unique_name(s, t.kind == "message" ? "hello" : t.kind);
        p.text = t.text_default;
        p.number = t.number_default;
        s.programs.push_back(p);
        changed = true;
    }
    return changed;
}

bool files_section(EditorContext& ctx, OsSettings& s) {
    if (!section(ctx, "Files", "Files that are already there when the OS starts. Try 'cat <file>' to read one."))
        return false;
    bool changed = false;
    int remove = -1;
    for (size_t i = 0; i < s.files.size(); ++i) {
        FileSpec& f = s.files[i];
        ImGui::PushID(static_cast<int>(i));
        row("Path", "Where the file lives, for example /home/notes.txt.");
        ImGui::SetNextItemWidth(-70.0f);
        changed |= ImGui::InputText("##path", &f.path);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
        changed |= ImGui::InputTextMultiline("##content", &f.content, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f));
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (remove >= 0) {
        s.files.erase(s.files.begin() + remove);
        changed = true;
    }
    if (ImGui::Button("Add file")) {
        s.files.push_back({unique_file(s), "Write anything here."});
        changed = true;
    }
    return changed;
}

void problems_box(EditorContext& ctx) {
    Project& project = *ctx.project;
    std::vector<std::string> problems = project.settings_problems();
    const HardwareParse& hw = project.hardware();
    for (const std::string& p : hw.problems) problems.push_back(p);
    if (!hw.found) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::InputBg));
        ImGui::BeginChild("##nodecl", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextColored(theme::color_vec(Color::Error), "This OS does not say what hardware it runs on yet.");
        if (ImGui::Button("Fix it: add the standard hardware")) {
            project.add_hardware_declaration();
            save_main(ctx);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (problems.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::InputBg));
    ImGui::BeginChild("##problems", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    for (const std::string& p : problems) {
        icons::item(icons::Glyph::Warning, ImGui::GetFontSize(), theme::color(Color::Warning));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", p.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void run_buttons(EditorContext& ctx) {
    const std::vector<BuildTarget> targets = ctx.project->build_targets();
    auto find = [&](const char* id) -> const BuildTarget* {
        for (const BuildTarget& t : targets)
            if (t.id == id) return &t;
        return nullptr;
    };
    const bool idle = !ctx.runner.running();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = std::floor((ImGui::GetContentRegionAvail().x - 2 * gap) / 3.0f);
    if (const BuildTarget* t = find("run");
        big_button("##try", icons::Glyph::Play, theme::color(Color::Implemented), "Try it", w, t && idle,
                   "Runs your OS right here as a normal app (fastest)."))
        ctx.run_target(*t);
    ImGui::SameLine();
    if (const BuildTarget* t = find("qemu");
        big_button("##vm", icons::Glyph::Play, IM_COL32(0x7F, 0xA8, 0xE8, 0xFF), "Run on a virtual PC", w, t && idle,
                   "Boots the real OS in a virtual PC (QEMU), exactly like a real computer would."))
        ctx.run_target(*t);
    ImGui::SameLine();
    if (const BuildTarget* t = find("iso");
        big_button("##iso", icons::Glyph::Build, theme::color(Color::Text), "Make bootable ISO", w, t && idle,
                   "Creates an .iso file in the build folder. Put it on a USB stick or open it in a virtual machine."))
        ctx.run_target(*t);
}

} // namespace

SettingsPanel::SettingsPanel() : state_(new State) {}
SettingsPanel::~SettingsPanel() { delete state_; }

void SettingsPanel::draw(EditorContext& ctx) {
    if (!ctx.project) {
        hint("Open or create an OS (File menu) to change its settings.");
        return;
    }
    Project& project = *ctx.project;
    if (ctx.fonts.ui_bold) ImGui::PushFont(ctx.fonts.ui_bold, ImGui::GetFontSize() * 1.35f);
    ImGui::TextUnformatted(project.name().c_str());
    if (ctx.fonts.ui_bold) ImGui::PopFont();
    hint("Change anything below: your OS is updated right away. Then press Try it.");
    ImGui::Spacing();
    run_buttons(ctx);
    problems_box(ctx);

    OsSettings& s = project.settings();
    bool changed = false;
    startup_section(ctx);
    changed |= look_section(ctx, s);
    hardware_section(ctx);
    changed |= programs_section(ctx, *state_, s);
    changed |= files_section(ctx, s);
    ImGui::Spacing();
    if (changed) project.settings_changed();

    ImGui::Spacing();
    ImGui::Separator();
    if (ctx.beginner_mode) {
        if (ImGui::Button("Show the code (advanced)")) {
            ctx.beginner_mode = false;
            ctx.request_focus_code = true;
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::SameLine();
        hint("Everything here is also plain C++ you can edit.");
    }
}

} // namespace osef::editor
