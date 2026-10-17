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
#include <cstdio>
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
    if (!section(ctx, "Boot & system", "OS identity, boot firmware and the minimum RAM the kernel requires.")) return;
    bool hw_changed = false;
    Component& os = project.root();
    if (Property* name = os.property("name")) {
        row("OS name", "OSEF_HARDWARE .name(): shown in the boot banner and the shell prompt (max 31 chars).");
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
            row("Boot firmware", "OSEF_HARDWARE .firmware(): the ISO gets only this boot path (El Torito BIOS or EFI system partition).");
            for (const EnumOption& o : p->options) {
                const bool uefi = o.label == "UEFI";
                const std::string label = uefi ? "UEFI (x86_64)" : "Legacy BIOS";
                if (ImGui::RadioButton(label.c_str(), p->i == o.value) && p->i != o.value) {
                    p->i = o.value;
                    project.apply_property(*fw, *p);
                    hw_changed = true;
                }
                ImGui::SetItemTooltip("%s", uefi ? "UEFI x86_64: Limine BOOTX64.EFI on an EFI system partition. Modern PCs and VMs (OVMF)."
                                                 : "Legacy BIOS: El Torito no-emulation boot via limine-bios-cd.bin. Older PCs, CSM, SeaBIOS.");
                if (&o != &p->options.back()) ImGui::SameLine();
            }
        }
    }
    if (Component* mem = project.find("hw.memory")) {
        if (Property* p = mem->property("memory_mb")) {
            row("Minimum RAM", "OSEF_HARDWARE .memory_mb(): the kernel panics at boot with less usable RAM; also the QEMU -m size.");
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
    if (!section(ctx, "Console appearance", "Framebuffer/terminal console theme (KernelConfig::appearance), ANSI 16-color palette.")) return false;
    bool changed = false;
    preview(ctx, s, ctx.project->name());
    ImGui::Spacing();

    row("Theme preset", "Predefined foreground/background/prompt color sets; each color can be overridden below.");
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
    row("Welcome banner", "Appearance::welcome: printed by the shell on start, before /etc/motd.");
    changed |= ImGui::InputTextMultiline("##welcome", &s.welcome, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.2f));
    row("Shell prompt", "Appearance::prompt template: {os} = OS name, {cwd} = working directory.");
    changed |= ImGui::InputText("##prompt", &s.prompt);
    row("Foreground (SGR 3x/9x)");
    changed |= color_combo("##text", s.text_color, 7);
    row("Background (SGR 4x/10x)");
    changed |= color_combo("##bg", s.background, 0);
    row("Prompt {os} color", "Color of the OS name in the prompt.");
    changed |= color_combo("##namec", s.prompt_name_color, 7);
    row("Prompt {cwd} color", "Color of the working directory in the prompt.");
    changed |= color_combo("##pathc", s.prompt_path_color, 7);
    row("Font scale", "Framebuffer console glyph scale (8x8 font); Automatic picks 2x at >= 1600 px width.");
    static const char* const sizes[] = {"Automatic", "Small (1x)", "Medium (2x)", "Large (3x)", "Huge (4x)"};
    changed |= ImGui::Combo("##size", &s.text_size, sizes, 5);
    return changed;
}

void hardware_section(EditorContext& ctx) {
    if (!section(ctx, "Devices & drivers", "OSEF_HARDWARE .devices(): drivers initialized at boot. Needs a console device (framebuffer or serial)."))
        return;
    struct Dev {
        const char* id;
        const char* label;
        const char* help;
    };
    static const Dev devices[] = {
        {"hw.display", "Framebuffer console", "Limine framebuffer, 32 bpp text console (Hw::Framebuffer)."},
        {"hw.keyboard", "PS/2 keyboard", "i8042 controller, scancode set 1, IRQ 1 (Hw::PS2Keyboard). USB keyboards via firmware emulation."},
        {"hw.serial", "Serial UART (COM1)", "16550 UART at 0x3F8, IRQ 4 (Hw::Serial). Console I/O and debugging; QEMU -serial stdio."},
        {"hw.rtc", "CMOS RTC", "MC146818-compatible real-time clock, wall-clock date/time (Hw::RTC)."},
        {"hw.pci", "PCI enumeration", "Configuration-space scan via ports 0xCF8/0xCFC (Hw::PCI); see the lspci command."},
        {"hw.usb", "USB (xHCI)", "xHCI host controller: device enumeration (lsusb) and HID boot keyboards (Hw::USB)."},
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
    for (const char* planned : {"PS/2 mouse (IRQ 12)", "USB mouse / mass storage", "Storage (AHCI / NVMe)", "Network (virtio-net / e1000)", "Audio (Intel HDA)"}) {
        bool off = false;
        ImGui::Checkbox(planned, &off);
        ImGui::SameLine(label_width());
        hint("Planned: no driver in the engine yet.");
    }
    ImGui::EndDisabled();
    if (changed) save_main(ctx);
}

bool programs_section(EditorContext& ctx, SettingsPanel::State& st, OsSettings& s) {
    if (!section(ctx, "Programs", "Processes registered in the ProgramRegistry, generated from templates into osef_generated.cpp."))
        return false;
    bool changed = false;
    changed |= ImGui::Checkbox("Shell (ShellModule)", &s.shell);
    ImGui::SetItemTooltip("Registers the 'sh' program: builtins like help, ls, ps, cat, hw, lspci.");

    row("Init process (PID 1)", "KernelConfig::init_program: the first process. The kernel halts when it exits.");
    const std::string current = s.start_program == "sh" ? "sh (shell)" : s.start_program;
    if (ImGui::BeginCombo("##start", current.c_str())) {
        if (ImGui::Selectable("sh (shell)", s.start_program == "sh")) {
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
        row("Program name", "Registry key and shell command: [a-z][a-z0-9_-]*, must not shadow a builtin.");
        const std::string before = p.name;
        if (ImGui::InputText("##name", &p.name)) {
            if (s.start_program == before) s.start_program = p.name;
            changed = true;
        }
        if (!is_valid_command_name(p.name))
            ImGui::TextColored(theme::color_vec(Color::Warning), "Use lowercase letters, numbers, - or _ (start with a letter).");
        row("Description", "Program::description, listed by `help`.");
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

bool interrupts_section(EditorContext& ctx, OsSettings& s) {
    if (!section(ctx, "Interrupts",
                 "kernel.interrupts(): deferred handlers for IRQ lines 0-15 (vector 32+n) and OS vectors "
                 "0x30-0xFF. Vectors 0-31 are CPU exceptions. Test soft vectors with the `int <vector>` command."))
        return false;
    bool changed = false;
    int remove = -1;
    for (size_t i = 0; i < s.interrupts.size(); ++i) {
        InterruptSpec& it = s.interrupts[i];
        ImGui::PushID(static_cast<int>(i) + 1000);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color(Color::PanelBg));
        ImGui::BeginChild("##irqcard", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        const bool irq = it.source == "irq";
        if (ctx.fonts.ui_bold) ImGui::PushFont(ctx.fonts.ui_bold, 0.0f);
        if (irq) ImGui::Text("IRQ %d  (vector 0x%02X)", it.number, 32 + it.number);
        else ImGui::Text("Vector 0x%02X", it.number);
        if (ctx.fonts.ui_bold) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);

        row("Source", "Software/OS vector (raised with `int` or by a device you program) or a legacy PIC IRQ line.");
        int source = irq ? 1 : 0;
        static const char* const sources[] = {"OS vector (0x30-0xFF)", "IRQ line (0-15)"};
        if (ImGui::Combo("##src", &source, sources, 2)) {
            it.source = source ? "irq" : "vector";
            it.number = source ? 12 : 0x80;
            changed = true;
        }
        row(irq ? "IRQ number" : "Vector", irq ? "0-15. The engine uses 0 (PIT), 1 (PS/2 keyboard) and 4 (COM1); "
                                                 "hooking them adds a handler, it does not replace the driver."
                                            : "48-255 (0x30-0xFF).");
        if (ImGui::InputInt("##num", &it.number, 1, 16, irq ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_CharsHexadecimal)) {
            it.number = irq ? std::clamp(it.number, 0, 15) : std::clamp(it.number, 48, 255);
            changed = true;
        }
        row("Name", "Label listed by the `irq` command.");
        changed |= ImGui::InputText("##iname", &it.name);
        row("Action", "What the handler does (in kernel context, on the next scheduler pass).");
        int action = it.action == "program" ? 1 : 0;
        static const char* const actions[] = {"Print a message", "Start a program"};
        if (ImGui::Combo("##act", &action, actions, 2)) {
            it.action = action ? "program" : "message";
            it.text = action ? (s.programs.empty() ? "sh" : s.programs.front().name) : "";
            changed = true;
        }
        if (it.action == "program") {
            row("Program");
            if (ImGui::BeginCombo("##prog", it.text.c_str())) {
                if (ImGui::Selectable("sh", it.text == "sh")) {
                    it.text = "sh";
                    changed = true;
                }
                for (const ProgramSpec& p : s.programs)
                    if (ImGui::Selectable(p.name.c_str(), it.text == p.name)) {
                        it.text = p.name;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        } else {
            row("Message");
            changed |= ImGui::InputText("##imsg", &it.text);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (remove >= 0) {
        s.interrupts.erase(s.interrupts.begin() + remove);
        changed = true;
    }
    ImGui::Spacing();
    if (ImGui::Button("Add interrupt handler")) {
        int v = 0x80;
        while (std::any_of(s.interrupts.begin(), s.interrupts.end(),
                           [&](const InterruptSpec& x) { return x.source == "vector" && x.number == v; }) && v < 255)
            ++v;
        char name[32];
        std::snprintf(name, sizeof name, "vector-%02x", v);
        s.interrupts.push_back({"vector", v, name, "message", "interrupt received"});
        changed = true;
    }
    return changed;
}

bool files_section(EditorContext& ctx, OsSettings& s) {
    if (!section(ctx, "Initial filesystem", "Files written to the root RamFS at boot (volatile; no persistent storage yet)."))
        return false;
    bool changed = false;
    int remove = -1;
    for (size_t i = 0; i < s.files.size(); ++i) {
        FileSpec& f = s.files[i];
        ImGui::PushID(static_cast<int>(i));
        row("Absolute path", "VFS path; parent directories are created, e.g. /home/notes.txt.");
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
        ImGui::TextColored(theme::color_vec(Color::Error), "main.cpp has no OSEF_HARDWARE declaration: the kernel will not link.");
        if (ImGui::Button("Insert default OSEF_HARDWARE declaration")) {
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
        big_button("##try", icons::Glyph::Play, theme::color(Color::Implemented), "Run hosted", w, t && idle,
                   "Builds and runs the OS as a native process on this machine (make run). Fastest iteration."))
        ctx.run_target(*t);
    ImGui::SameLine();
    if (const BuildTarget* t = find("qemu");
        big_button("##vm", icons::Glyph::Play, IM_COL32(0x7F, 0xA8, 0xE8, 0xFF), "Boot in QEMU", w, t && idle,
                   "Builds the freestanding x86_64 kernel + ISO and boots it in QEMU with the declared firmware (make run-iso)."))
        ctx.run_target(*t);
    ImGui::SameLine();
    if (const BuildTarget* t = find("iso");
        big_button("##iso", icons::Glyph::Build, theme::color(Color::Text), "Build ISO image", w, t && idle,
                   "Links build/<os>.elf and packages build/<os>.iso with Limine + xorriso (make iso). Hybrid image: dd to USB or attach to a VM."))
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
    hint("Edits update OSEF_HARDWARE in main.cpp and regenerate osef_generated.cpp immediately.");
    ImGui::Spacing();
    run_buttons(ctx);
    problems_box(ctx);

    OsSettings& s = project.settings();
    bool changed = false;
    startup_section(ctx);
    changed |= look_section(ctx, s);
    hardware_section(ctx);
    changed |= programs_section(ctx, *state_, s);
    changed |= interrupts_section(ctx, s);
    changed |= files_section(ctx, s);
    ImGui::Spacing();
    if (changed) project.settings_changed();

    ImGui::Spacing();
    ImGui::Separator();
    if (ctx.beginner_mode) {
        if (ImGui::Button("Show source (main.cpp, osef_generated.cpp)")) {
            ctx.beginner_mode = false;
            ctx.request_focus_code = true;
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::SameLine();
        hint("All settings compile to plain C++; edit it directly in the Code panel.");
    }
}

} // namespace osef::editor
