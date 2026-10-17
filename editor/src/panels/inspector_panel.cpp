// Inspector: every detail of the selected component, as Unity-style
// collapsible sections. Edits are committed through Project::apply_property /
// apply_enabled, which rewrite the OSEF_HARDWARE declaration in main.cpp.

#include "panels/inspector_panel.h"

#include "app/editor_context.h"
#include "app/icons.h"
#include "app/theme.h"
#include "model/component.h"
#include "model/hardware_decl.h"
#include "model/project.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace osef::editor {

using theme::Color;

namespace {

constexpr float kPad = 10.0f;          // left/right content margin
constexpr float kHeaderIcon = 32.0f;
constexpr float kSectionIcon = 14.0f;
constexpr const char* kBoundTooltip =
    "Bound to OSEF_HARDWARE in main.cpp \xE2\x80\x94 edits rewrite the declaration";
constexpr const char* kHardwareSection = "Hardware Declaration";

ImU32 col(Color c) { return theme::color(c); }

ImU32 with_alpha(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

// Font helpers: sizes are relative to the UI font so global scaling applies once.
struct FontSet {
    ImFont* ui = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    ImFont* small = nullptr;
    float ui_base = 15.0f;     // unscaled size for PushFont
    float small_base = 12.0f;

    explicit FontSet(const Fonts& f) {
        ui = f.ui ? f.ui : ImGui::GetFont();
        bold = f.ui_bold ? f.ui_bold : ui;
        mono = f.mono ? f.mono : ui;
        small = f.small ? f.small : ui;
        ui_base = ImGui::GetStyle().FontSizeBase;
        small_base = f.small && f.ui && f.ui->LegacySize > 0.0f ? ui_base * f.small->LegacySize / f.ui->LegacySize
                                                              : ui_base * 0.8f;
    }
};

// Screen-space x range of the content (inside the scrollbar, minus margins).
float content_left() { return ImGui::GetCurrentWindow()->InnerRect.Min.x + kPad; }
float content_right() { return ImGui::GetCurrentWindow()->InnerRect.Max.x - kPad; }
float content_width() { return std::max(40.0f, content_right() - content_left()); }
void push_wrap() { ImGui::PushTextWrapPos(content_right() - ImGui::GetWindowPos().x); }

void text_dim(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::TextDim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void open_source(EditorContext& ctx, const SourceRef& ref) {
    ctx.open_file(ctx.project->resolve(ref), ref.line);
    ctx.request_focus_code = true;
}

void create_driver_stub(EditorContext& ctx, const Component& c) {
    std::string error;
    const std::filesystem::path path = ctx.project->create_driver_stub(c, &error);
    if (path.empty()) {
        ctx.log.error("Create Driver Stub for " + c.name + " failed: " + (error.empty() ? "unknown error" : error));
        ctx.request_focus_console = true;
        return;
    }
    ctx.log.info("Created driver stub for " + c.name + ": " + path.string());
    ctx.open_file(path);
    ctx.request_focus_code = true;
}

std::string value_text(const Property& p) {
    switch (p.type) {
    case PropertyType::Bool: return p.b ? "on" : "off";
    case PropertyType::Int: return std::to_string(p.i) + (p.unit.empty() ? "" : " " + p.unit);
    case PropertyType::Enum:
        for (const EnumOption& o : p.options)
            if (o.value == p.i) return o.label;
        return std::to_string(p.i);
    case PropertyType::String:
    case PropertyType::Info: return p.s;
    case PropertyType::Resolution:
        return p.i == 0 && p.i2 == 0 ? "Auto" : std::to_string(p.i) + " x " + std::to_string(p.i2);
    }
    return {};
}

// Defaults of the osef::Hardware() builder for editable properties, where one
// exists (see engine/include/osef/hardware.h). Returns false when unknown.
bool hardware_default(const Property& p, Property& out) {
    if (p.read_only || !p.bound) return false;
    const std::string_view id = p.id;
    out = p;
    if (p.type == PropertyType::Resolution) {
        out.i = out.i2 = 0;
        return true;
    }
    if (p.type == PropertyType::Int && id.find("memory") != std::string_view::npos) {
        out.i = p.max > p.min ? std::clamp<int64_t>(128, p.min, p.max) : 128;
        return true;
    }
    if (p.type == PropertyType::Int && id.find("timeout") != std::string_view::npos) {
        out.i = std::max<int64_t>(0, p.min);
        return true;
    }
    if ((p.type == PropertyType::Enum || p.type == PropertyType::Int) && id.find("baud") != std::string_view::npos) {
        out.i = 115200;
        return true;
    }
    return false;
}

// Re-flows code lines longer than `max_cols` by breaking before " | " or after
// ", " with a hanging indent, so the declaration stays readable (and valid
// C++) in a narrow Inspector. Lines without a break point are left alone.
std::string fit_code_lines(const std::string& text, size_t max_cols) {
    if (max_cols < 24) return text;
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        const size_t indent = line.find_first_not_of(' ') == std::string::npos ? 0 : line.find_first_not_of(' ');
        const std::string cont(indent + 4, ' ');
        while (line.size() > max_cols) {
            size_t cut = std::string::npos;
            for (size_t i = std::min(max_cols, line.size() - 1); i > indent + 8; --i) {
                if (line.compare(i, 3, " | ") == 0) { cut = i; break; }
                if (line.compare(i - 1, 2, ", ") == 0) { cut = i; break; }
            }
            if (cut == std::string::npos) break;
            out += line.substr(0, cut) + "\n";
            size_t rest = cut;
            while (rest < line.size() && line[rest] == ' ') ++rest;
            line = cont + line.substr(rest);
        }
        out += line;
        if (end < text.size()) out += "\n";
        start = end + 1;
    }
    return out;
}

int64_t clamp_int(const Property& p, int64_t v) {
    if (p.max > p.min) v = std::clamp(v, p.min, p.max);
    return v;
}

// Three vertical dots (the section "more" menu).
void draw_kebab(ImDrawList* dl, ImVec2 center, ImU32 color) {
    for (int i = -1; i <= 1; ++i)
        dl->AddCircleFilled(ImVec2(center.x, center.y + static_cast<float>(i) * 4.0f), 1.3f, color, 8);
}

void draw_badge(ImDrawList* dl, ImFont* font, float size, ImVec2 right_center, const char* text, ImU32 color) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float h = std::round(ts.y + 4.0f), pad = 7.0f;
    const ImVec2 min(std::floor(right_center.x - ts.x - pad * 2.0f), std::floor(right_center.y - h * 0.5f));
    const ImVec2 max(std::floor(right_center.x), min.y + h);
    dl->AddRectFilled(min, max, with_alpha(color, 0.18f), h * 0.5f);
    dl->AddRect(min, max, with_alpha(color, 0.55f), h * 0.5f);
    dl->AddText(font, size, ImVec2(min.x + pad, min.y + std::floor((h - ts.y) * 0.5f)), color, text);
}

float badge_width(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x + 14.0f;
}

struct InspectorState {
    std::unordered_map<std::string, bool> section_open;  // per section title (Unity keeps foldouts per type)

    // In-progress edit of one field, committed on Enter or focus loss.
    std::string edit_key;
    int64_t edit_i = 0;
    std::string edit_s;
    int64_t drag_start = 0;  // label drag (Unity-style scrubbing of Int fields)

    std::string last_component;

    bool& open(const std::string& key) { return section_open.try_emplace(key, true).first->second; }
};

using State = InspectorState;
void property_row(EditorContext& ctx, State& st, Component& c, Property& p);

struct SectionOptions {
    ComponentKind kind = ComponentKind::Group;
    bool use_kind = false;
    icons::Glyph glyph = icons::Glyph::File;
    bool bound = false;
    std::function<void()> menu;  // contents of the ⋮ popup, or empty for no menu
};

// Unity component header: full-width bar, 1 px top border, arrow, icon, bold
// title, optional link mark and ⋮ menu. Returns whether the section is open.
bool section_header(State& st, const FontSet& fonts, const std::string& title, const SectionOptions& opt) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float x0 = window->InnerRect.Min.x, x1 = window->InnerRect.Max.x;
    const float h = std::round(ImGui::GetFontSize() + 8.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    bool& open = st.open(title);

    ImGui::PushID(title.c_str());
    ImGui::SetNextItemAllowOverlap();
    const bool pressed = ImGui::InvisibleButton("##header", ImVec2(std::max(1.0f, x1 - x0), h));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 after = ImVec2(window->DC.CursorPos.x, window->DC.CursorPos.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(x1, pos.y + h), col(hovered ? Color::SectionHeaderHover : Color::SectionHeader));
    dl->AddLine(ImVec2(x0, pos.y + 0.5f), ImVec2(x1, pos.y + 0.5f), col(Color::SectionBorder));

    const float cy = std::floor(pos.y + h * 0.5f) + 0.5f;
    const float ax = x0 + 10.0f;
    const ImU32 arrow_col = col(Color::TextDim);
    if (open)
        dl->AddTriangleFilled(ImVec2(ax - 4.0f, cy - 2.0f), ImVec2(ax + 4.0f, cy - 2.0f), ImVec2(ax, cy + 2.5f), arrow_col);
    else
        dl->AddTriangleFilled(ImVec2(ax - 2.0f, cy - 4.0f), ImVec2(ax + 2.5f, cy), ImVec2(ax - 2.0f, cy + 4.0f), arrow_col);

    const ImVec2 icon_pos(x0 + 20.0f, std::floor(cy - kSectionIcon * 0.5f));
    if (opt.use_kind) icons::draw(dl, icon_pos, kSectionIcon, opt.kind, icons::kind_color(opt.kind));
    else icons::draw(dl, icon_pos, kSectionIcon, opt.glyph, col(Color::TextDim));

    const float font_size = ImGui::GetFontSize();
    dl->AddText(fonts.bold, font_size, ImVec2(x0 + 20.0f + kSectionIcon + 6.0f, std::floor(cy - font_size * 0.5f)),
                col(Color::Text), title.c_str());

    // Right side: ⋮ menu, then the link mark to its left.
    float right = x1 - 4.0f;
    if (opt.menu) {
        const float w = 20.0f;
        ImGui::SetCursorScreenPos(ImVec2(right - w, pos.y));
        if (ImGui::InvisibleButton("##menu", ImVec2(w, h))) ImGui::OpenPopup("##section_menu");
        const bool mh = ImGui::IsItemHovered();
        if (mh) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(b.x - 2.0f, b.y - 2.0f), col(Color::Hover), 3.0f);
        }
        draw_kebab(dl, ImVec2(std::floor(right - w * 0.5f) + 0.5f, cy), col(mh ? Color::Text : Color::TextDim));
        ImGui::SetItemTooltip("More");
        right -= w + 2.0f;
        if (ImGui::BeginPopup("##section_menu")) {
            opt.menu();
            ImGui::EndPopup();
        }
    }
    if (opt.bound) {
        const float w = 18.0f, g = 14.0f;
        ImGui::SetCursorScreenPos(ImVec2(right - w, pos.y));
        ImGui::InvisibleButton("##bound", ImVec2(w, h));
        const bool bh = ImGui::IsItemHovered();
        icons::draw(dl, ImVec2(std::floor(right - w * 0.5f - g * 0.5f), std::floor(cy - g * 0.5f)), g,
                    icons::Glyph::Link, col(bh ? Color::Accent : Color::TextDim));
        ImGui::SetItemTooltip("%s", kBoundTooltip);
    }
    ImGui::PopID();

    if (pressed) open = !open;
    ImGui::SetCursorScreenPos(after);
    ImGui::Dummy(ImVec2(0.0f, open ? 3.0f : 0.0f));
    return open;
}

void section_end(bool open) {
    if (open) ImGui::Dummy(ImVec2(0.0f, 5.0f));
}

// ---- property rows ----------------------------------------------------------------------

struct RowLayout {
    float x_label = 0.0f;   // screen x of the label
    float label_w = 0.0f;
    float x_ctrl = 0.0f;    // screen x of the control
    float ctrl_w = 0.0f;
};

RowLayout row_layout() {
    RowLayout r;
    r.x_label = content_left();
    const float w = content_width();
    r.label_w = std::floor(std::clamp(w * 0.4f, std::min(80.0f, w * 0.5f), 220.0f));
    r.x_ctrl = r.x_label + r.label_w;
    r.ctrl_w = std::max(30.0f, content_right() - r.x_ctrl);
    return r;
}

// Draws the label column; returns true when it is being dragged (Int scrubbing).
bool property_label(const Property& p, const RowLayout& r, bool scrub) {
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 pos(r.x_label, ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##label", ImVec2(r.label_w - 4.0f, frame_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (scrub && (hovered || active)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = col(p.read_only ? Color::TextDim : Color::Text);
    const float ty = pos.y + ImGui::GetStyle().FramePadding.y;
    const ImVec2 ts = ImGui::CalcTextSize(p.label.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
    ImGui::RenderTextEllipsis(dl, ImVec2(pos.x, ty), ImVec2(pos.x + r.label_w - 6.0f, ty + ts.y + 2.0f),
                              pos.x + r.label_w - 6.0f, p.label.c_str(), nullptr, &ts);
    ImGui::PopStyleColor();

    if (!active && ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(p.label.c_str());
        if (!p.tooltip.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::TextDim));
            ImGui::TextUnformatted(p.tooltip.c_str());
            ImGui::PopStyleColor();
        }
        if (p.bound) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::Accent));
            ImGui::TextUnformatted("Bound to OSEF_HARDWARE in main.cpp");
            ImGui::PopStyleColor();
        }
        if (p.read_only) ImGui::TextDisabled("Read-only");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SetCursorScreenPos(ImVec2(r.x_ctrl, pos.y));
    return scrub && active;
}

// Read-only text that can still be selected and copied.
void selectable_text(const char* id, const std::string& text, float width, ImFont* font = nullptr, float font_base = 0.0f,
                     bool framed = false) {
    if (font) ImGui::PushFont(font, font_base);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pad = framed ? ImVec2(8.0f, 6.0f) : ImVec2(0.0f, style.FramePadding.y);
    const float wrap_w = std::max(10.0f, width - pad.x * 2.0f - 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_w);
    const float height = std::max(ts.y, ImGui::GetTextLineHeight()) + pad.y * 2.0f + 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, framed ? 1.0f : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, framed ? theme::color_vec(Color::CodeBg) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::color_vec(Color::InputBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    std::string copy = text;
    ImGui::InputTextMultiline(id, copy.data(), copy.size() + 1, ImVec2(width, height),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap |
                                  ImGuiInputTextFlags_NoUndoRedo);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (font) ImGui::PopFont();
}

void commit(EditorContext& ctx, Component& c, Property& p) { ctx.project->apply_property(c, p); }

void quick_setup(EditorContext& ctx, State& st, const FontSet& fonts, Component& os) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::Accent));
    ImGui::TextUnformatted("Set up your OS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Choose a name, boot options, and the devices this OS can use.");
    ImGui::Spacing();

    if (Property* name = os.property("name")) property_row(ctx, st, os, *name);
    if (Component* firmware = ctx.project->find("boot.firmware")) {
        if (Property* p = firmware->property("firmware")) {
            ImGui::TextUnformatted("Boot with");
            ImGui::SameLine();
            for (const EnumOption& option : p->options) {
                ImGui::PushID(static_cast<int>(option.value));
                const bool selected = p->i == option.value;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, theme::color(Color::Selection));
                if (ImGui::Button(option.label.c_str()) && !selected) {
                    p->i = option.value;
                    commit(ctx, *firmware, *p);
                }
                if (selected) ImGui::PopStyleColor();
                ImGui::PopID();
                if (!option.tooltip.empty()) ImGui::SetItemTooltip("%s", option.tooltip.c_str());
                if (&option != &p->options.back()) ImGui::SameLine();
            }
        }
    }
    if (Component* memory = ctx.project->find("hw.memory")) {
        if (Property* p = memory->property("memory_mb")) property_row(ctx, st, *memory, *p);
    }

    ImGui::SeparatorText("Devices");
    ImGui::TextWrapped("Select the built-in devices your OS should use. Console devices are required.");
    const char* const device_ids[] = {"hw.serial", "hw.display", "hw.keyboard", "hw.rtc", "hw.pci"};
    for (const char* id : device_ids) {
        Component* device = ctx.project->find(id);
        if (!device || !device->toggleable) continue;
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              theme::color(device->enabled ? Color::Selection : Color::Button));
        const std::string label = std::string(device->enabled ? "Enabled: " : "Add: ") + device->name;
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
            device->enabled = !device->enabled;
            ctx.project->apply_enabled(*device);
        }
        ImGui::PopStyleColor();
        if (!device->summary.empty()) ImGui::SetItemTooltip("%s", device->summary.c_str());
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Show code and advanced controls")) {
        ctx.beginner_mode = false;
        ctx.request_focus_code = true;
        ImGui::MarkIniSettingsDirty();
    }
    if (ctx.fonts.small) ImGui::PushFont(ctx.fonts.small, fonts.small_base);
    text_dim("Your changes are previewed here. Use File > Save to keep them.");
    if (ctx.fonts.small) ImGui::PopFont();
}

void property_row(EditorContext& ctx, State& st, Component& c, Property& p) {
    const RowLayout r = row_layout();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frame_h = ImGui::GetFrameHeight();
    const std::string key = c.id + "." + p.id;
    ImGui::PushID(p.id.c_str());
    const bool editable = !p.read_only;
    const bool scrubbing = property_label(p, r, p.type == PropertyType::Int && editable);

    switch (p.type) {
    case PropertyType::Info: {
        selectable_text("##value", p.s.empty() ? std::string("\xE2\x80\x94") : p.s, r.ctrl_w);
        break;
    }
    case PropertyType::Bool: {
        ImGui::BeginDisabled(!editable);
        bool v = p.b;
        if (ImGui::Checkbox("##value", &v) && v != p.b) {
            p.b = v;
            commit(ctx, c, p);
        }
        ImGui::EndDisabled();
        break;
    }
    case PropertyType::Int: {
        // Label drag scrubs the value (one step per 4 px), committed on release.
        if (scrubbing) {
            if (ImGui::IsItemActivated() || st.edit_key != key + "#drag") {
                st.edit_key = key + "#drag";
                st.drag_start = p.i;
            }
            const float dx = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
            st.edit_i = clamp_int(p, st.drag_start + p.step * static_cast<int64_t>(dx / 4.0f));
        } else if (st.edit_key == key + "#drag") {
            st.edit_key.clear();
            if (st.edit_i != p.i) {
                p.i = st.edit_i;
                commit(ctx, c, p);
            }
        }
        const bool dragging = st.edit_key == key + "#drag";
        const bool editing = st.edit_key == key;
        int64_t v = dragging || editing ? st.edit_i : p.i;

        const float unit_w = p.unit.empty() ? 0.0f : ImGui::CalcTextSize(p.unit.c_str()).x + style.ItemInnerSpacing.x;
        const float btn = frame_h;
        const float field_w = std::max(30.0f, r.ctrl_w - unit_w - (editable ? (btn + 2.0f) * 2.0f : 0.0f));
        ImGui::BeginDisabled(!editable);
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputScalar("##value", ImGuiDataType_S64, &v, nullptr, nullptr, "%lld");
        if (ImGui::IsItemActive()) {
            st.edit_key = key;
            st.edit_i = v;
        } else if (editing) {
            st.edit_key.clear();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                v = clamp_int(p, v);
                if (v != p.i) {
                    p.i = v;
                    commit(ctx, c, p);
                }
            }
        }
        if (p.min < p.max) ImGui::SetItemTooltip("%lld to %lld%s%s", static_cast<long long>(p.min),
                                                  static_cast<long long>(p.max), p.unit.empty() ? "" : " ",
                                                  p.unit.c_str());
        if (editable) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, style.FramePadding.y));
            const int64_t step = std::max<int64_t>(1, p.step);
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("-", ImVec2(btn, btn))) {
                const int64_t nv = clamp_int(p, p.i - step);
                if (nv != p.i) {
                    p.i = nv;
                    commit(ctx, c, p);
                }
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("+", ImVec2(btn, btn))) {
                const int64_t nv = clamp_int(p, p.i + step);
                if (nv != p.i) {
                    p.i = nv;
                    commit(ctx, c, p);
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndDisabled();
        if (!p.unit.empty()) {
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            ImGui::AlignTextToFramePadding();
            text_dim(p.unit.c_str());
        }
        break;
    }
    case PropertyType::Enum: {
        ImGui::BeginDisabled(!editable);
        const EnumOption* current = nullptr;
        for (const EnumOption& o : p.options)
            if (o.value == p.i) current = &o;
        if (p.radio) {
            bool first = true;
            for (const EnumOption& o : p.options) {
                if (!first) ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
                first = false;
                if (ImGui::RadioButton(o.label.c_str(), o.value == p.i) && o.value != p.i) {
                    p.i = o.value;
                    commit(ctx, c, p);
                }
                if (!o.tooltip.empty()) ImGui::SetItemTooltip("%s", o.tooltip.c_str());
            }
        } else {
            const std::string preview = current ? current->label : std::to_string(p.i);
            const float unit_w = p.unit.empty() ? 0.0f : ImGui::CalcTextSize(p.unit.c_str()).x + style.ItemInnerSpacing.x;
            ImGui::SetNextItemWidth(std::max(30.0f, r.ctrl_w - unit_w));
            if (ImGui::BeginCombo("##value", preview.c_str())) {
                for (const EnumOption& o : p.options) {
                    const bool sel = o.value == p.i;
                    if (ImGui::Selectable(o.label.c_str(), sel) && !sel) {
                        p.i = o.value;
                        commit(ctx, c, p);
                    }
                    if (!o.tooltip.empty()) ImGui::SetItemTooltip("%s", o.tooltip.c_str());
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!p.unit.empty()) {
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
                text_dim(p.unit.c_str());
            }
        }
        ImGui::EndDisabled();
        break;
    }
    case PropertyType::String: {
        ImGui::BeginDisabled(!editable);
        const bool editing = st.edit_key == key;
        std::string v = editing ? st.edit_s : p.s;
        ImGui::SetNextItemWidth(r.ctrl_w);
        ImGui::InputText("##value", &v);
        if (ImGui::IsItemActive()) {
            st.edit_key = key;
            st.edit_s = v;
        } else if (editing) {
            st.edit_key.clear();
            if (ImGui::IsItemDeactivatedAfterEdit() && v != p.s) {
                p.s = v;
                commit(ctx, c, p);
            }
        }
        ImGui::EndDisabled();
        break;
    }
    case PropertyType::Resolution: {
        ImGui::BeginDisabled(!editable);
        bool automatic = p.i == 0 && p.i2 == 0;
        const float auto_w = frame_h + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto").x;
        const float x_w = ImGui::CalcTextSize("\xC3\x97").x + style.ItemInnerSpacing.x * 2.0f;
        const float field_w = std::max(36.0f, std::floor((r.ctrl_w - auto_w - x_w - style.ItemSpacing.x * 2.0f) * 0.5f));
        ImGui::BeginDisabled(automatic);
        for (int k = 0; k < 2; ++k) {
            const std::string fkey = key + (k == 0 ? ".w" : ".h");
            int64_t& field = k == 0 ? p.i : p.i2;
            const bool editing = st.edit_key == fkey;
            int64_t v = editing ? st.edit_i : field;
            if (k == 1) {
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
                text_dim("\xC3\x97");
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            }
            ImGui::SetNextItemWidth(field_w);
            ImGui::InputScalar(k == 0 ? "##w" : "##h", ImGuiDataType_S64, &v, nullptr, nullptr, "%lld");
            ImGui::SetItemTooltip(k == 0 ? "Width in pixels" : "Height in pixels");
            if (ImGui::IsItemActive()) {
                st.edit_key = fkey;
                st.edit_i = v;
            } else if (editing) {
                st.edit_key.clear();
                v = std::clamp<int64_t>(v, 0, 16384);
                if (ImGui::IsItemDeactivatedAfterEdit() && v != field) {
                    field = v;
                    commit(ctx, c, p);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
        if (ImGui::Checkbox("Auto", &automatic)) {
            if (automatic) {
                p.i = p.i2 = 0;
            } else {
                p.i = 1024;
                p.i2 = 768;
            }
            commit(ctx, c, p);
        }
        ImGui::SetItemTooltip("Use the firmware's default mode (0 x 0)");
        ImGui::EndDisabled();
        break;
    }
    }
    ImGui::PopID();

    if (ctx.beginner_mode && !p.tooltip.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(content_left(), ImGui::GetCursorScreenPos().y));
        push_wrap();
        text_dim(p.tooltip.c_str());
        ImGui::PopTextWrapPos();
    }
}

// ---- header -------------------------------------------------------------------------

const char* status_label(ComponentStatus s) {
    switch (s) {
    case ComponentStatus::Implemented: return "Implemented";
    case ComponentStatus::Partial: return "Partial";
    case ComponentStatus::Planned: return "Planned";
    }
    return "";
}

Color status_color(ComponentStatus s) {
    switch (s) {
    case ComponentStatus::Implemented: return Color::Implemented;
    case ComponentStatus::Partial: return Color::Partial;
    case ComponentStatus::Planned: return Color::Planned;
    }
    return Color::Planned;
}

void draw_header(EditorContext& ctx, const FontSet& fonts, Component& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float top = ImGui::GetCursorScreenPos().y + 8.0f;
    const float x0 = content_left(), x1 = content_right();
    const bool dim_icon = c.status == ComponentStatus::Planned || (c.toggleable && !c.enabled);
    icons::draw(dl, ImVec2(x0, top), kHeaderIcon, c.kind, with_alpha(icons::kind_color(c.kind), dim_icon ? 0.5f : 1.0f));

    const float x_text = x0 + kHeaderIcon + 10.0f;
    const float frame_h = ImGui::GetFrameHeight();
    float x = x_text;

    // Line 1: [enable] Name ............ [status]
    if (c.toggleable) {
        ImGui::SetCursorScreenPos(ImVec2(x, top - 2.0f));
        bool v = c.enabled;
        if (ImGui::Checkbox("##enabled", &v) && v != c.enabled) {
            c.enabled = v;
            ctx.project->apply_enabled(c);
        }
        ImGui::SetItemTooltip("%s", c.toggle_tooltip.empty() ? (c.enabled ? "Enabled" : "Disabled")
                                                             : c.toggle_tooltip.c_str());
        x += frame_h + 6.0f;
    }
    const bool show_badge = c.kind != ComponentKind::Os && c.kind != ComponentKind::Group;
    const float badge_size = std::round(fonts.small_base * ImGui::GetStyle().FontScaleDpi * ImGui::GetStyle().FontScaleMain);
    const char* badge = status_label(c.status);
    const float badge_w = show_badge ? badge_width(fonts.small, badge_size, badge) : 0.0f;
    const float name_size = std::round(ImGui::GetFontSize() * 1.2f);
    const float line1_cy = top - 2.0f + frame_h * 0.5f;
    const ImVec2 name_ts = fonts.bold->CalcTextSizeA(name_size, FLT_MAX, 0.0f, c.name.c_str());
    const float name_right = x1 - (show_badge ? badge_w + 8.0f : 0.0f);
    dl->PushClipRect(ImVec2(x, top - 4.0f), ImVec2(std::max(x, name_right), top + frame_h + 4.0f), true);
    dl->AddText(fonts.bold, name_size, ImVec2(x, std::floor(line1_cy - name_ts.y * 0.5f)),
                col(c.toggleable && !c.enabled ? Color::TextDim : Color::Text), c.name.c_str());
    dl->PopClipRect();
    if (show_badge) draw_badge(dl, fonts.small, badge_size, ImVec2(x1, line1_cy), badge, col(status_color(c.status)));

    // Line 2: summary; line 3: id; line 4: breadcrumb.
    ImGui::SetCursorScreenPos(ImVec2(x_text, top + frame_h + 1.0f));
    const float indent = x_text - window->Pos.x - window->DC.Indent.x;
    ImGui::Indent(indent);
    if (!c.summary.empty()) {
        push_wrap();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::TextDim));
        ImGui::TextWrapped("%s", c.summary.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::PushFont(fonts.mono, fonts.small_base);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::TextDim));
    ImGui::TextUnformatted(c.id.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetItemTooltip("Component id (Copy from the Hierarchy context menu)");

    // Breadcrumb: ancestors are clickable.
    std::vector<Component*> chain;
    for (Component* p = &c; p; p = p->parent) chain.push_back(p);
    std::reverse(chain.begin(), chain.end());
    if (chain.size() > 1) {
        ImGui::PushFont(fonts.small, fonts.small_base);
        const float sep_w = ImGui::CalcTextSize(" \xE2\x80\xBA ").x;
        bool first = true;
        for (Component* p : chain) {
            const float w = ImGui::CalcTextSize(p->name.c_str()).x;
            if (!first) {
                const bool wrap = ImGui::GetCursorScreenPos().x + sep_w + w > x1 + 1.0f;
                ImGui::SameLine(0.0f, 0.0f);
                if (wrap) ImGui::NewLine();
                text_dim(" \xE2\x80\xBA ");
                ImGui::SameLine(0.0f, 0.0f);
            }
            first = false;
            if (p == &c) {
                text_dim(p->name.c_str());
                continue;
            }
            ImGui::PushID(p->id.c_str());
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##crumb", ImVec2(std::max(1.0f, w), ImGui::GetTextLineHeight())))
                ctx.select(p->id);
            const bool h = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddText(pos, col(h ? Color::Text : Color::TextDim), p->name.c_str());
            if (h) {
                const float uy = std::floor(pos.y + ImGui::GetTextLineHeight()) - 0.5f;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, uy), ImVec2(pos.x + w, uy), col(Color::Text));
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::PopID();
        }
        ImGui::PopFont();
    }
    ImGui::Unindent(indent);
    const float bottom = std::max(ImGui::GetCursorScreenPos().y, top + kHeaderIcon + 4.0f) + 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(window->Pos.x, bottom));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// "Planned" callout with the driver stub button.
void draw_planned_callout(EditorContext& ctx, Component& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = content_left(), x1 = content_right();
    const float pad = 9.0f, icon = 16.0f;
    const float y0 = ImGui::GetCursorScreenPos().y + 2.0f;

    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    icons::draw(dl, ImVec2(x0 + pad, y0 + pad), icon, icons::Glyph::Info, col(Color::Info));
    const float tx = x0 + pad + icon + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(tx, y0 + pad));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x1 - pad - ImGui::GetWindowPos().x);
    ImGui::TextUnformatted("Planned \xE2\x80\x94 not implemented in the engine yet. This is a place to expand OSEF.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (ImGui::Button("Create Driver Stub\xE2\x80\xA6")) create_driver_stub(ctx, c);
    ImGui::SetItemTooltip("Writes drivers/%s.cpp into the OS project as a starting point", c.id.c_str());
    ImGui::EndGroup();
    const float y1 = ImGui::GetItemRectMax().y + pad;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), with_alpha(col(Color::Info), 0.10f), 4.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), with_alpha(col(Color::Info), 0.40f), 4.0f);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, y1 + 8.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---- sections -------------------------------------------------------------------------

std::string copy_values(const Component& c, const std::string& section) {
    std::string out;
    for (const Property& p : c.properties)
        if ((p.section.empty() ? "Properties" : p.section) == section) out += p.label + ": " + value_text(p) + "\n";
    return out;
}

void hardware_declaration_block(EditorContext& ctx, const FontSet& fonts) {
    Project& project = *ctx.project;
    const HardwareParse& hw = project.hardware();
    const float x0 = content_left(), w = content_width();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line_h = ImGui::GetTextLineHeight();

    auto problem_row = [&](const std::string& text, Color color, icons::Glyph glyph) {
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        icons::draw(dl, ImVec2(pos.x, pos.y + std::floor((line_h - 14.0f) * 0.5f)), 14.0f, glyph, col(color));
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.0f, pos.y));
        push_wrap();
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
    };

    if (!hw.found) {
        problem_row("main.cpp has no OSEF_HARDWARE declaration. An OSEF OS must declare its machine, or it "
                    "fails to link.",
                    Color::Warning, icons::Glyph::Warning);
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + 4.0f));
        if (ImGui::Button("Add Hardware Declaration")) project.add_hardware_declaration();
        ImGui::SetItemTooltip("Inserts a default declaration after #include <osef/hardware.h>");
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + 2.0f));
    ImGui::PushFont(fonts.mono, 0.0f);
    const float char_w = std::max(1.0f, ImGui::CalcTextSize("M").x);
    ImGui::PopFont();
    const size_t cols = static_cast<size_t>(std::max(0.0f, (w - 18.0f) / char_w));
    selectable_text("##decl", fit_code_lines(generate_hardware_decl(hw.decl), cols), w, fonts.mono, 0.0f, true);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (hw.problems.empty()) {
        problem_row("The declaration is complete.", Color::Implemented, icons::Glyph::Check);
    } else {
        for (const std::string& problem : hw.problems) problem_row(problem, Color::Warning, icons::Glyph::Warning);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + 4.0f));
    if (ImGui::Button("Show in Code")) {
        ctx.open_file(project.main_file(), hw.line);
        ctx.request_focus_code = true;
    }
    ImGui::SetItemTooltip("Opens main.cpp at line %d", hw.line);
}

void sources_section(EditorContext& ctx, State& st, const FontSet& fonts, Component& c) {
    SectionOptions opt;
    opt.glyph = icons::Glyph::FileCode;
    if (!c.sources.empty())
        opt.menu = [&]() {
            if (ImGui::MenuItem("Copy paths")) {
                std::string out;
                for (const SourceRef& s : c.sources) out += ctx.project->resolve(s).string() + "\n";
                ImGui::SetClipboardText(out.c_str());
            }
        };
    const bool open = section_header(st, fonts, "Source Files", opt);
    if (!open) return;
    if (c.sources.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(content_left(), ImGui::GetCursorScreenPos().y));
        text_dim(c.status == ComponentStatus::Planned ? "None yet \xE2\x80\x94 create a driver stub to start one."
                                                      : "No source files.");
        section_end(open);
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float row_h = std::round(ImGui::GetFontSize() + 7.0f);
    const float font_size = ImGui::GetFontSize();
    const float small_size = std::round(fonts.small_base * ImGui::GetStyle().FontScaleDpi * ImGui::GetStyle().FontScaleMain);
    int index = 0;
    for (const SourceRef& ref : c.sources) {
        const std::filesystem::path abs = ctx.project->resolve(ref);
        std::error_code ec;
        const bool exists = std::filesystem::exists(abs, ec);
        const float xa = window->InnerRect.Min.x, xb = window->InnerRect.Max.x;
        const ImVec2 pos(xa, ImGui::GetCursorScreenPos().y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(index++);
        if (ImGui::InvisibleButton("##src", ImVec2(std::max(1.0f, xb - xa), row_h))) open_source(ctx, ref);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) dl->AddRectFilled(pos, ImVec2(xb, pos.y + row_h), col(Color::Hover));
        const float cy = std::floor(pos.y + row_h * 0.5f);
        const float x = content_left();
        icons::draw(dl, ImVec2(x, cy - 8.0f), 16.0f, icons::Glyph::FileCode,
                    col(exists ? Color::SynMacro : Color::TextDim));
        const std::string name = std::filesystem::path(ref.path).filename().string();
        const std::string label = ref.label.empty() ? name : ref.label;
        const float tx = x + 22.0f;
        const float right = content_right() - (hovered ? 20.0f : 0.0f);
        const ImVec2 lts = ImGui::CalcTextSize(label.c_str());
        dl->PushClipRect(ImVec2(tx, pos.y), ImVec2(std::max(tx, right), pos.y + row_h), true);
        dl->AddText(ImVec2(tx, std::floor(cy - font_size * 0.5f)), col(Color::Text), label.c_str());
        std::string shown = ref.path + (ref.line > 0 ? ":" + std::to_string(ref.line) : "") +
                            (exists ? "" : "  (missing)");
        // Long paths lose their leading directories ("…/osef-image/main.cpp") instead of the file name.
        const float path_x = tx + lts.x + 8.0f, path_w = right - path_x;
        if (path_w > 0.0f && fonts.small->CalcTextSizeA(small_size, FLT_MAX, 0.0f, shown.c_str()).x > path_w) {
            static const char kEllipsis[] = "\xE2\x80\xA6";
            size_t cut = 0;
            while (cut < shown.size()) {
                cut = shown.find('/', cut + 1);
                if (cut == std::string::npos) break;
                const std::string cand = kEllipsis + shown.substr(cut);
                if (fonts.small->CalcTextSizeA(small_size, FLT_MAX, 0.0f, cand.c_str()).x <= path_w) {
                    shown = cand;
                    break;
                }
            }
        }
        dl->AddText(fonts.small, small_size, ImVec2(path_x, std::floor(cy - small_size * 0.5f)),
                    col(exists ? Color::TextDim : Color::Error), shown.c_str());
        dl->PopClipRect();
        if (hovered) {
            icons::draw(dl, ImVec2(content_right() - 14.0f, cy - 7.0f), 14.0f, icons::Glyph::ExternalLink,
                        col(Color::Text));
            ImGui::SetTooltip("%s%s\nClick to open in the code editor", abs.string().c_str(),
                              ref.line > 0 ? (":" + std::to_string(ref.line)).c_str() : "");
        }
    }
    section_end(open);
}

void documentation_section(State& st, const FontSet& fonts, const Component& c) {
    if (c.description.empty()) return;
    SectionOptions opt;
    opt.glyph = icons::Glyph::File;
    opt.menu = [&]() {
        if (ImGui::MenuItem("Copy text")) ImGui::SetClipboardText(c.description.c_str());
    };
    const bool open = section_header(st, fonts, "Documentation", opt);
    if (!open) return;
    push_wrap();
    size_t start = 0;
    bool first = true;
    while (start <= c.description.size()) {
        size_t end = c.description.find("\n\n", start);
        if (end == std::string::npos) end = c.description.size();
        const std::string para = c.description.substr(start, end - start);
        if (!para.empty()) {
            if (!first) ImGui::Dummy(ImVec2(0.0f, 3.0f));
            first = false;
            ImGui::SetCursorScreenPos(ImVec2(content_left(), ImGui::GetCursorScreenPos().y));
            ImGui::TextUnformatted(para.c_str());
        }
        start = end + 2;
    }
    ImGui::PopTextWrapPos();
    section_end(open);
}

} // namespace

struct InspectorPanel::State : InspectorState {};

InspectorPanel::InspectorPanel() : state_(new State()) {}
InspectorPanel::~InspectorPanel() { delete state_; }

void InspectorPanel::draw(EditorContext& ctx) {
    State& st = *state_;
    const FontSet fonts(ctx.fonts);
    Component* c = ctx.project ? ctx.selected() : nullptr;

    if (!c) {
        const char* text = ctx.project
                               ? (ctx.beginner_mode
                                      ? "Choose a part from Hierarchy to learn what it does and adjust its settings."
                                      : "Select a component in the Hierarchy")
                               : (ctx.beginner_mode ? "Open an example or project from File > Open Project to get started."
                                                    : "No project open");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float icon = 32.0f;
        const float y = origin.y + std::max(20.0f, avail.y * 0.35f);
        icons::draw(ImGui::GetWindowDrawList(), ImVec2(std::floor(origin.x + (avail.x - icon) * 0.5f), y), icon,
                    ComponentKind::Os, with_alpha(col(Color::TextDim), 0.6f));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(kPad, (avail.x - ts.x) * 0.5f), y + icon + 10.0f));
        text_dim(text);
        return;
    }
    if (c->id != st.last_component) {
        st.edit_key.clear();
        st.last_component = c->id;
    }

    if (c->id == "os" && ctx.beginner_mode) {
        ImGui::PushID(c->id.c_str());
        quick_setup(ctx, st, fonts, *c);
        ImGui::PopID();
        return;
    }

    ImGui::PushID(c->id.c_str());
    draw_header(ctx, fonts, *c);
    if (ctx.beginner_mode) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color_vec(Color::Accent));
        ImGui::TextUnformatted("Getting started");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Change settings below, save your project, then choose Run to try your OS.");
        ImGui::Spacing();
    }
    if (c->status == ComponentStatus::Planned) draw_planned_callout(ctx, *c);

    // One section per distinct Property::section, in order of first appearance.
    std::vector<std::string> sections;
    for (const Property& p : c->properties) {
        const std::string s = p.section.empty() ? "Properties" : p.section;
        if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
    }
    const bool is_os = c->kind == ComponentKind::Os;
    if (is_os && std::find(sections.begin(), sections.end(), kHardwareSection) == sections.end())
        sections.insert(sections.begin(), kHardwareSection);

    for (const std::string& section : sections) {
        SectionOptions opt;
        opt.use_kind = true;
        opt.kind = c->kind;
        std::vector<Property*> props;
        bool any_default = false;
        for (Property& p : c->properties) {
            if ((p.section.empty() ? "Properties" : p.section) != section) continue;
            props.push_back(&p);
            opt.bound |= p.bound;
            Property d;
            if (hardware_default(p, d)) any_default = true;
        }
        const bool decl_block = is_os && section == kHardwareSection;
        if (decl_block) opt.bound = true;
        opt.menu = [&, section]() {
            if (ImGui::MenuItem("Copy values", nullptr, false, !props.empty()))
                ImGui::SetClipboardText(copy_values(*c, section).c_str());
            if (any_default && ImGui::MenuItem("Reset to defaults")) {
                for (Property* p : props) {
                    Property d;
                    if (!hardware_default(*p, d)) continue;
                    if (d.i == p->i && d.i2 == p->i2 && d.b == p->b && d.s == p->s) continue;
                    p->i = d.i;
                    p->i2 = d.i2;
                    p->b = d.b;
                    p->s = d.s;
                    commit(ctx, *c, *p);
                }
            }
            if (decl_block) {
                const HardwareParse& hw = ctx.project->hardware();
                if (ImGui::MenuItem("Copy declaration", nullptr, false, hw.found))
                    ImGui::SetClipboardText(generate_hardware_decl(hw.decl).c_str());
                if (ImGui::MenuItem("Show in Code", nullptr, false, hw.found)) {
                    ctx.open_file(ctx.project->main_file(), hw.line);
                    ctx.request_focus_code = true;
                }
            }
        };
        const bool open = section_header(st, fonts, section, opt);
        if (!open) continue;
        ImGui::PushID(section.c_str());
        for (Property* p : props) property_row(ctx, st, *c, *p);
        if (decl_block) {
            if (!props.empty()) ImGui::Dummy(ImVec2(0.0f, 2.0f));
            hardware_declaration_block(ctx, fonts);
        }
        ImGui::PopID();
        section_end(open);
    }

    sources_section(ctx, st, fonts, *c);
    documentation_section(st, fonts, *c);

    // Closing border under the last section, then some air.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(window->InnerRect.Min.x, y + 0.5f),
                                        ImVec2(window->InnerRect.Max.x, y + 0.5f), col(Color::SectionBorder));
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::PopID();
}

} // namespace osef::editor
