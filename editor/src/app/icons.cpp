// Vector icons drawn with ImDrawList. Every icon is designed on a 16 x 16
// grid (stroke coordinates on .5 so 1.5 px lines land on pixel centers at
// 16 px) and scaled to the requested size, with stroke endpoints snapped to
// pixel centers so small sizes stay crisp.

#include "app/icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace osef::editor::icons {
namespace {

constexpr float kPi = 3.14159265358979f;

float rad(float degrees) { return degrees * kPi / 180.0f; }

ImU32 with_alpha(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

// Dark ink for marks drawn on top of filled glyphs (info "i", warning "!").
constexpr ImU32 kInk = IM_COL32(0x26, 0x26, 0x26, 0xFF);

// Pen: maps 16-grid coordinates into the target square.
struct Pen {
    ImDrawList* dl;
    ImVec2 o;      // integer origin
    float s;       // pixels per grid unit
    float t;       // stroke thickness in pixels
    ImU32 c;

    Pen(ImDrawList* d, ImVec2 min, float size, ImU32 color)
        : dl(d), o(std::floor(min.x), std::floor(min.y)), s(size / 16.0f),
          t(std::max(1.0f, 1.5f * size / 16.0f)), c(color) {}

    // Free point (curves, diagonals).
    ImVec2 p(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    // Stroke point snapped to the nearest pixel center.
    ImVec2 ps(float x, float y) const {
        return ImVec2(std::floor(o.x + x * s) + 0.5f, std::floor(o.y + y * s) + 0.5f);
    }
    // Fill point snapped to the nearest pixel corner.
    ImVec2 pf(float x, float y) const {
        return ImVec2(std::round(o.x + x * s), std::round(o.y + y * s));
    }
    float r(float v) const { return v * s; }

    void line(float x0, float y0, float x1, float y1, ImU32 col = 0) const {
        dl->PathLineTo(ps(x0, y0));
        dl->PathLineTo(ps(x1, y1));
        dl->PathStroke(col ? col : c, t, 0);
    }
    // Diagonal line without snapping (avoids kinks).
    void dline(float x0, float y0, float x1, float y1, ImU32 col = 0, float thick = 0.0f) const {
        dl->PathLineTo(p(x0, y0));
        dl->PathLineTo(p(x1, y1));
        dl->PathStroke(col ? col : c, thick > 0.0f ? thick : t, 0);
    }
    void poly(std::initializer_list<ImVec2> pts, bool closed, bool snap = true) const {
        for (const ImVec2& v : pts) dl->PathLineTo(snap ? ps(v.x, v.y) : p(v.x, v.y));
        dl->PathStroke(c, t, closed ? ImDrawFlags_Closed : 0);
    }
    void fill_poly(std::initializer_list<ImVec2> pts, ImU32 col = 0) const {
        ImVec2 buf[24];
        int n = 0;
        for (const ImVec2& v : pts)
            if (n < 24) buf[n++] = p(v.x, v.y);
        dl->AddConcavePolyFilled(buf, n, col ? col : c);
    }
    void rect(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
        dl->PathRect(ps(x0, y0), ps(x1, y1), r(rounding));
        dl->PathStroke(c, t, ImDrawFlags_Closed);
    }
    void fill_rect(float x0, float y0, float x1, float y1, float rounding = 0.0f, ImU32 col = 0,
                   ImDrawFlags flags = 0) const {
        dl->AddRectFilled(pf(x0, y0), pf(x1, y1), col ? col : c, r(rounding), flags);
    }
    void circle(float cx, float cy, float radius, ImU32 col = 0) const {
        dl->AddCircle(p(cx, cy), r(radius), col ? col : c, 0, t);
    }
    void fill_circle(float cx, float cy, float radius, ImU32 col = 0) const {
        dl->AddCircleFilled(p(cx, cy), r(radius), col ? col : c);
    }
    void arc(float cx, float cy, float radius, float deg0, float deg1) const {
        dl->PathArcTo(p(cx, cy), r(radius), rad(deg0), rad(deg1));
        dl->PathStroke(c, t, 0);
    }
    // A chevron / arrow head made of two strokes meeting at (x1, y1).
    void chevron(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->PathLineTo(p(x0, y0));
        dl->PathLineTo(p(x1, y1));
        dl->PathLineTo(p(x2, y2));
        dl->PathStroke(c, t, 0);
    }
};

ImU32 fill_tint(const Pen& pen, float alpha) { return with_alpha(pen.c, alpha); }

// ---- shared shapes ------------------------------------------------------------------

void folder(const Pen& pen) {
    pen.fill_poly({{1.5f, 3.5f}, {6.0f, 3.5f}, {7.5f, 5.0f}, {14.5f, 5.0f}, {14.5f, 13.5f}, {1.5f, 13.5f}},
                  fill_tint(pen, 0.22f));
    pen.poly({{1.5f, 3.5f}, {6.0f, 3.5f}, {7.5f, 5.0f}, {14.5f, 5.0f}, {14.5f, 13.5f}, {1.5f, 13.5f}}, true);
}

void folder_open(const Pen& pen) {
    pen.poly({{12.5f, 7.0f}, {12.5f, 5.0f}, {7.5f, 5.0f}, {6.0f, 3.5f}, {1.5f, 3.5f}, {1.5f, 13.5f}}, false);
    pen.fill_poly({{1.5f, 13.5f}, {4.0f, 7.5f}, {15.0f, 7.5f}, {12.5f, 13.5f}}, fill_tint(pen, 0.3f));
    pen.poly({{1.5f, 13.5f}, {4.0f, 7.5f}, {15.0f, 7.5f}, {12.5f, 13.5f}}, true, false);
}

void page(const Pen& pen) {
    pen.poly({{3.5f, 1.5f}, {9.5f, 1.5f}, {12.5f, 4.5f}, {12.5f, 14.5f}, {3.5f, 14.5f}}, true);
    pen.poly({{9.5f, 1.5f}, {9.5f, 4.5f}, {12.5f, 4.5f}}, false);
}

// Rectangle chip body with `n` pins per listed side.
void chip_pins(const Pen& pen, float x0, float y0, float x1, float y1, float len,
               std::initializer_list<float> xs, std::initializer_list<float> ys) {
    for (float x : xs) {
        pen.line(x, y0 - len, x, y0);
        pen.line(x, y1, x, y1 + len);
    }
    for (float y : ys) {
        pen.line(x0 - len, y, x0, y);
        pen.line(x1, y, x1 + len, y);
    }
}

// Capsule outline from a to b (link glyph).
void capsule(const Pen& pen, ImVec2 a, ImVec2 b, float radius) {
    const float ang = std::atan2(b.y - a.y, b.x - a.x);
    pen.dl->PathArcTo(pen.p(a.x, a.y), pen.r(radius), ang + kPi * 0.5f, ang + kPi * 1.5f);
    pen.dl->PathArcTo(pen.p(b.x, b.y), pen.r(radius), ang - kPi * 0.5f, ang + kPi * 0.5f);
    pen.dl->PathStroke(pen.c, pen.t, ImDrawFlags_Closed);
}

// ---- component kinds ----------------------------------------------------------------

void draw_os(const Pen& pen) {
    // Unity-like cube: three visible faces, top lightest.
    const ImVec2 top(8.0f, 1.5f), ur(14.0f, 4.75f), lr(14.0f, 11.25f), bot(8.0f, 14.5f), ll(2.0f, 11.25f),
        ul(2.0f, 4.75f), mid(8.0f, 8.0f);
    pen.fill_poly({top, ur, mid, ul}, fill_tint(pen, 0.55f));
    pen.fill_poly({ul, mid, bot, ll}, fill_tint(pen, 0.22f));
    pen.fill_poly({mid, ur, lr, bot}, fill_tint(pen, 0.08f));
    auto P = [&](ImVec2 v) { return pen.p(v.x, v.y); };
    const ImVec2 hex[] = {P(top), P(ur), P(lr), P(bot), P(ll), P(ul)};
    pen.dl->AddPolyline(hex, 6, pen.c, pen.t, ImDrawFlags_Closed);
    pen.dl->PathLineTo(P(ul));
    pen.dl->PathLineTo(P(mid));
    pen.dl->PathLineTo(P(ur));
    pen.dl->PathStroke(pen.c, pen.t, 0);
    pen.dl->PathLineTo(P(mid));
    pen.dl->PathLineTo(P(bot));
    pen.dl->PathStroke(pen.c, pen.t, 0);
}

void draw_firmware(const Pen& pen) {
    // ROM chip carrying a "boot" up-arrow.
    pen.rect(3.5f, 1.5f, 12.5f, 14.5f, 1.0f);
    chip_pins(pen, 3.5f, 1.5f, 12.5f, 14.5f, 2.0f, {}, {5.5f, 10.5f});
    pen.fill_poly({{8.0f, 3.75f}, {11.0f, 7.5f}, {5.0f, 7.5f}});
    pen.fill_rect(7.0f, 7.0f, 9.0f, 12.0f);
}

void draw_cpu(const Pen& pen) {
    pen.rect(3.5f, 3.5f, 12.5f, 12.5f, 1.0f);
    pen.fill_rect(6.0f, 6.0f, 10.0f, 10.0f, 0.5f);
    chip_pins(pen, 3.5f, 3.5f, 12.5f, 12.5f, 2.0f, {6.0f, 10.0f}, {6.0f, 10.0f});
}

void draw_memory(const Pen& pen) {
    // RAM stick: board, three chips, contact pins with a key notch.
    pen.rect(1.5f, 3.5f, 14.5f, 10.5f, 0.5f);
    pen.fill_rect(3.5f, 5.5f, 5.5f, 8.5f);
    pen.fill_rect(7.0f, 5.5f, 9.0f, 8.5f);
    pen.fill_rect(10.5f, 5.5f, 12.5f, 8.5f);
    for (float x : {3.5f, 5.5f, 7.5f, 10.5f, 12.5f}) pen.line(x, 10.5f, x, 13.0f);
}

void draw_interrupts(const Pen& pen) {
    pen.fill_poly({{10.25f, 1.0f}, {3.0f, 9.25f}, {7.5f, 9.25f}, {5.75f, 15.0f}, {13.0f, 6.75f}, {8.5f, 6.75f}});
}

void draw_timer(const Pen& pen) {
    // Stopwatch: dial, crown, side button, hand.
    pen.circle(8.0f, 9.25f, 5.5f);
    pen.line(8.0f, 1.5f, 8.0f, 3.5f);
    pen.line(6.0f, 1.5f, 10.0f, 1.5f);
    pen.dline(12.25f, 4.25f, 13.75f, 2.75f);
    pen.line(8.0f, 9.25f, 8.0f, 6.0f);
    pen.fill_circle(8.0f, 9.25f, 1.1f);
}

void draw_clock(const Pen& pen) {
    pen.circle(8.0f, 8.0f, 6.5f);
    pen.line(8.0f, 8.0f, 8.0f, 4.0f);
    pen.dline(8.0f, 8.0f, 10.75f, 9.75f);
    pen.fill_circle(8.0f, 8.0f, 1.0f);
}

void draw_serial(const Pen& pen) {
    // D-sub connector: trapezoid shell with two staggered pin rows.
    pen.poly({{1.5f, 3.5f}, {14.5f, 3.5f}, {12.5f, 12.5f}, {3.5f, 12.5f}}, true, false);
    for (float x : {4.5f, 6.83f, 9.17f, 11.5f}) pen.fill_circle(x, 6.5f, 0.95f);
    for (float x : {5.67f, 8.0f, 10.33f}) pen.fill_circle(x, 9.5f, 0.95f);
}

void draw_display(const Pen& pen) {
    pen.rect(1.5f, 2.5f, 14.5f, 11.5f, 1.0f);
    pen.fill_rect(3.5f, 4.5f, 12.5f, 9.5f, 0.0f, fill_tint(pen, 0.25f));
    pen.line(8.0f, 11.5f, 8.0f, 13.5f);
    pen.line(4.5f, 14.0f, 11.5f, 14.0f);
}

void draw_keyboard(const Pen& pen) {
    pen.rect(1.5f, 3.5f, 14.5f, 12.5f, 1.5f);
    for (float x : {3.5f, 5.75f, 8.0f, 10.25f, 12.5f}) {
        pen.fill_rect(x - 0.75f, 5.25f, x + 0.75f, 6.75f);
        pen.fill_rect(x - 0.75f, 7.75f, x + 0.75f, 9.25f);
    }
    pen.fill_rect(5.0f, 10.0f, 11.0f, 11.25f);
}

void draw_mouse(const Pen& pen) {
    pen.rect(3.5f, 1.5f, 12.5f, 14.5f, 4.5f);
    pen.line(3.5f, 7.0f, 12.5f, 7.0f);
    pen.line(8.0f, 1.5f, 8.0f, 7.0f);
    pen.fill_rect(7.0f, 3.25f, 9.0f, 5.75f, 1.0f);
}

void draw_usb(const Pen& pen) {
    // USB trident: arrow tip, stem, round and square branch ends, base disc.
    pen.fill_poly({{8.0f, 0.75f}, {10.25f, 4.0f}, {5.75f, 4.0f}});
    pen.line(8.0f, 3.5f, 8.0f, 12.0f);
    pen.fill_circle(8.0f, 13.0f, 2.0f);
    pen.poly({{8.0f, 11.0f}, {4.0f, 8.5f}, {4.0f, 6.5f}}, false, false);
    pen.fill_circle(4.0f, 5.75f, 1.5f);
    pen.poly({{8.0f, 9.0f}, {12.0f, 6.75f}, {12.0f, 5.5f}}, false, false);
    pen.fill_rect(10.5f, 3.0f, 13.5f, 6.0f);
}

void draw_storage(const Pen& pen) {
    // Disk cylinder with a middle band.
    const float rx = 5.75f, ry = 2.0f;
    pen.dl->AddEllipse(pen.p(8.0f, 4.0f), ImVec2(pen.r(rx), pen.r(ry)), pen.c, 0.0f, 0, pen.t);
    pen.line(2.25f, 4.0f, 2.25f, 12.0f);
    pen.line(13.75f, 4.0f, 13.75f, 12.0f);
    pen.dl->PathEllipticalArcTo(pen.p(8.0f, 8.0f), ImVec2(pen.r(rx), pen.r(ry)), 0.0f, 0.0f, kPi);
    pen.dl->PathStroke(pen.c, pen.t, 0);
    pen.dl->PathEllipticalArcTo(pen.p(8.0f, 12.0f), ImVec2(pen.r(rx), pen.r(ry)), 0.0f, 0.0f, kPi);
    pen.dl->PathStroke(pen.c, pen.t, 0);
}

void draw_pci(const Pen& pen) {
    // Expansion card: mounting bracket, board, chip, gold fingers.
    pen.line(1.5f, 1.5f, 1.5f, 14.5f);
    pen.line(1.5f, 1.5f, 3.5f, 1.5f);
    pen.rect(1.5f, 3.5f, 14.5f, 10.5f);
    pen.fill_rect(5.0f, 5.0f, 9.0f, 9.0f);
    pen.fill_rect(10.5f, 5.0f, 12.5f, 7.0f);
    for (float x : {5.5f, 7.5f, 9.5f, 12.5f}) pen.line(x, 10.5f, x, 13.0f);
}

void draw_network(const Pen& pen) {
    pen.dline(8.0f, 3.5f, 3.5f, 12.0f);
    pen.dline(8.0f, 3.5f, 12.5f, 12.0f);
    pen.dline(3.5f, 12.0f, 12.5f, 12.0f);
    pen.fill_circle(8.0f, 3.5f, 2.4f);
    pen.fill_circle(3.5f, 12.0f, 2.4f);
    pen.fill_circle(12.5f, 12.0f, 2.4f);
}

void draw_audio(const Pen& pen) {
    pen.fill_poly({{1.5f, 5.75f}, {4.5f, 5.75f}, {8.0f, 2.5f}, {8.0f, 13.5f}, {4.5f, 10.25f}, {1.5f, 10.25f}});
    pen.arc(8.0f, 8.0f, 3.0f, -50.0f, 50.0f);
    pen.arc(8.0f, 8.0f, 6.0f, -50.0f, 50.0f);
}

void draw_power(const Pen& pen) {
    pen.arc(8.0f, 8.75f, 5.75f, -55.0f, 235.0f);
    pen.line(8.0f, 1.5f, 8.0f, 8.0f);
}

void draw_scheduler(const Pen& pen) {
    // Gantt chart: time axis plus three staggered task bars.
    pen.line(1.5f, 1.5f, 1.5f, 14.5f);
    pen.fill_rect(3.0f, 2.5f, 9.0f, 5.0f, 0.75f);
    pen.fill_rect(6.0f, 6.75f, 14.5f, 9.25f, 0.75f);
    pen.fill_rect(4.0f, 11.0f, 11.0f, 13.5f, 0.75f);
}

void draw_processes(const Pen& pen) {
    // Two stacked windows; only the visible part of the back one is drawn.
    pen.poly({{5.5f, 4.5f}, {5.5f, 1.5f}, {14.5f, 1.5f}, {14.5f, 9.5f}, {11.5f, 9.5f}}, false);
    pen.rect(1.5f, 5.5f, 11.5f, 14.5f, 0.5f);
    pen.fill_rect(1.5f, 5.5f, 11.5f, 8.0f);
}

void draw_ipc(const Pen& pen) {
    pen.line(1.5f, 5.0f, 13.5f, 5.0f);
    pen.chevron(10.75f, 2.25f, 13.5f, 5.0f, 10.75f, 7.75f);
    pen.line(2.5f, 11.0f, 14.5f, 11.0f);
    pen.chevron(5.25f, 8.25f, 2.5f, 11.0f, 5.25f, 13.75f);
}

void draw_syscalls(const Pen& pen) {
    // "Enter the kernel": an arrow going through a door frame.
    pen.poly({{8.5f, 1.5f}, {14.5f, 1.5f}, {14.5f, 14.5f}, {8.5f, 14.5f}}, false);
    pen.line(1.5f, 8.0f, 10.5f, 8.0f);
    pen.chevron(7.5f, 5.0f, 10.5f, 8.0f, 7.5f, 11.0f);
}

void draw_file_system(const Pen& pen) {
    // Folder tree: root folder, trunk, two child folders.
    pen.fill_rect(1.0f, 1.0f, 8.0f, 5.0f, 1.0f);
    pen.line(3.5f, 5.0f, 3.5f, 12.5f);
    pen.line(3.5f, 8.5f, 7.0f, 8.5f);
    pen.line(3.5f, 12.5f, 7.0f, 12.5f);
    pen.fill_rect(7.0f, 7.0f, 15.0f, 10.0f, 0.75f);
    pen.fill_rect(7.0f, 11.0f, 15.0f, 14.0f, 0.75f);
}

void draw_devices(const Pen& pen) {
    // Power plug: prongs, body, cord.
    pen.line(5.5f, 1.0f, 5.5f, 4.5f);
    pen.line(10.5f, 1.0f, 10.5f, 4.5f);
    pen.fill_rect(3.0f, 4.0f, 13.0f, 6.0f);
    pen.fill_poly({{3.5f, 5.5f}, {12.5f, 5.5f}, {12.5f, 7.5f}, {9.5f, 10.75f}, {6.5f, 10.75f}, {3.5f, 7.5f}});
    pen.line(8.0f, 10.0f, 8.0f, 15.0f);
}

void draw_shell(const Pen& pen) {
    pen.rect(1.5f, 2.5f, 14.5f, 13.5f, 1.0f);
    pen.chevron(4.25f, 5.75f, 6.75f, 8.25f, 4.25f, 10.75f);
    pen.line(8.0f, 10.5f, 11.5f, 10.5f);
}

void draw_program(const Pen& pen) {
    // App window with a title bar and two control dots.
    pen.rect(1.5f, 2.5f, 14.5f, 13.5f, 1.0f);
    pen.fill_rect(1.5f, 2.5f, 14.5f, 6.0f, 1.0f, 0, ImDrawFlags_RoundTop);
    pen.fill_circle(3.75f, 4.25f, 0.85f, kInk);
    pen.fill_circle(6.0f, 4.25f, 0.85f, kInk);
}

void draw_programs(const Pen& pen) {
    // App grid (launcher): four rounded tiles.
    for (float y : {1.5f, 8.5f})
        for (float x : {1.5f, 8.5f}) pen.fill_rect(x, y, x + 6.0f, y + 6.0f, 1.25f);
}

void draw_module(const Pen& pen) {
    // Solid puzzle piece: square body, round knobs on top and right.
    ImDrawList* dl = pen.dl;
    const float kr = 2.5f, neck = 1.3f;
    const float off = std::sqrt(kr * kr - neck * neck);
    const ImVec2 top_c(6.5f, 5.0f - off), right_c(11.0f + off, 9.5f);
    auto ang = [](ImVec2 c, float x, float y) { return std::atan2(y - c.y, x - c.x); };
    constexpr int kSeg = 14;
    dl->PathLineTo(pen.p(2.0f, 5.0f));
    dl->PathLineTo(pen.p(top_c.x - neck, 5.0f));
    dl->PathArcTo(pen.p(top_c.x, top_c.y), pen.r(kr), ang(top_c, top_c.x - neck, 5.0f),
                  ang(top_c, top_c.x + neck, 5.0f) + 2.0f * kPi, kSeg);
    dl->PathLineTo(pen.p(11.0f, 5.0f));
    dl->PathLineTo(pen.p(11.0f, right_c.y - neck));
    dl->PathArcTo(pen.p(right_c.x, right_c.y), pen.r(kr), ang(right_c, 11.0f, right_c.y - neck) + 2.0f * kPi,
                  ang(right_c, 11.0f, right_c.y + neck) + 2.0f * kPi, kSeg);
    dl->PathLineTo(pen.p(11.0f, 14.0f));
    dl->PathLineTo(pen.p(2.0f, 14.0f));
    dl->PathFillConcave(pen.c);
}

void draw_modules(const Pen& pen) { draw_module(pen); }

// ---- glyphs -------------------------------------------------------------------------

void draw_build(const Pen& pen) {
    // Hammer, designed upright and rotated 45 degrees clockwise.
    const float a = rad(45.0f), ca = std::cos(a), sa = std::sin(a);
    auto rot = [&](float x, float y) {
        const float dx = x - 8.0f, dy = y - 8.0f;
        return ImVec2(8.0f + dx * ca - dy * sa - 1.0f, 8.0f + dx * sa + dy * ca + 0.75f);
    };
    pen.fill_poly({rot(3.5f, 2.0f), rot(12.5f, 2.0f), rot(12.5f, 5.5f), rot(3.5f, 5.5f)});
    pen.fill_poly({rot(7.0f, 5.0f), rot(9.0f, 5.0f), rot(9.0f, 14.5f), rot(7.0f, 14.5f)});
}

void draw_test(const Pen& pen) {
    // Erlenmeyer flask, half full.
    pen.fill_poly({{4.25f, 9.5f}, {11.75f, 9.5f}, {13.5f, 13.0f}, {2.5f, 13.0f}}, fill_tint(pen, 0.55f));
    pen.poly({{6.0f, 1.5f}, {6.0f, 6.0f}, {2.5f, 13.0f}, {2.5f, 14.0f}, {13.5f, 14.0f}, {13.5f, 13.0f},
              {10.0f, 6.0f}, {10.0f, 1.5f}},
             false, false);
    pen.line(4.5f, 1.5f, 11.5f, 1.5f);
}

void draw_gear(const Pen& pen) {
    // Solid gear with a dark hub hole.
    ImDrawList* dl = pen.dl;
    const ImVec2 c = pen.p(8.0f, 8.0f);
    const float ro = pen.r(7.25f), ri = pen.r(5.1f);
    constexpr int kTeeth = 8;
    for (int k = 0; k < kTeeth; ++k) {
        const float th = static_cast<float>(k) * 2.0f * kPi / kTeeth;
        const float a[4] = {th - 0.36f, th - 0.17f, th + 0.17f, th + 0.36f};
        const float rr[4] = {ri, ro, ro, ri};
        for (int j = 0; j < 4; ++j) dl->PathLineTo(ImVec2(c.x + std::cos(a[j]) * rr[j], c.y + std::sin(a[j]) * rr[j]));
    }
    dl->PathFillConcave(pen.c);
    pen.fill_circle(8.0f, 8.0f, 2.3f, kInk);
}

void draw_link(const Pen& pen) {
    capsule(pen, ImVec2(4.25f, 11.75f), ImVec2(6.75f, 9.25f), 2.5f);
    capsule(pen, ImVec2(9.25f, 6.75f), ImVec2(11.75f, 4.25f), 2.5f);
    pen.dline(6.25f, 9.75f, 9.75f, 6.25f);
}

void draw_lock(const Pen& pen, bool open) {
    pen.fill_rect(2.5f, 7.0f, 13.5f, 14.5f, 1.5f);
    if (open) {
        pen.poly({{4.75f, 7.5f}, {4.75f, 4.5f}}, false);
        pen.arc(8.0f, 4.5f, 3.25f, 180.0f, 360.0f);
        pen.line(11.25f, 4.5f, 11.25f, 5.0f);
    } else {
        pen.line(4.75f, 7.0f, 4.75f, 5.25f);
        pen.line(11.25f, 7.0f, 11.25f, 5.25f);
        pen.arc(8.0f, 5.25f, 3.25f, 180.0f, 360.0f);
    }
    pen.fill_circle(8.0f, 10.75f, 1.35f, kInk);
}

void draw_external_link(const Pen& pen) {
    pen.poly({{7.0f, 3.5f}, {2.5f, 3.5f}, {2.5f, 13.5f}, {12.5f, 13.5f}, {12.5f, 9.0f}}, false);
    pen.dline(7.0f, 9.0f, 13.5f, 2.5f);
    pen.poly({{9.0f, 2.5f}, {13.5f, 2.5f}, {13.5f, 7.0f}}, false);
}

void draw_file_code(const Pen& pen) {
    page(pen);
    pen.chevron(7.0f, 7.25f, 5.25f, 9.5f, 7.0f, 11.75f);
    pen.chevron(9.0f, 7.25f, 10.75f, 9.5f, 9.0f, 11.75f);
}

void draw_info(const Pen& pen) {
    pen.fill_circle(8.0f, 8.0f, 7.0f);
    pen.fill_circle(8.0f, 4.6f, 1.15f, kInk);
    pen.fill_rect(7.0f, 6.75f, 9.0f, 12.25f, 0.0f, kInk);
}

void draw_warning(const Pen& pen) {
    pen.dl->PathLineTo(pen.p(8.0f, 1.0f));
    pen.dl->PathLineTo(pen.p(15.25f, 14.25f));
    pen.dl->PathLineTo(pen.p(0.75f, 14.25f));
    pen.dl->PathFillConvex(pen.c);
    pen.fill_rect(7.0f, 5.25f, 9.0f, 10.0f, 0.0f, kInk);
    pen.fill_rect(7.0f, 11.25f, 9.0f, 13.0f, 0.0f, kInk);
}

void draw_error(const Pen& pen) {
    pen.fill_circle(8.0f, 8.0f, 7.0f);
    pen.dline(5.25f, 5.25f, 10.75f, 10.75f, kInk, pen.t * 1.2f);
    pen.dline(10.75f, 5.25f, 5.25f, 10.75f, kInk, pen.t * 1.2f);
}

} // namespace

void draw(ImDrawList* dl, ImVec2 min, float size, Glyph glyph, ImU32 color) {
    if (!dl || size <= 0.0f) return;
    const Pen pen(dl, min, size, color);
    switch (glyph) {
    case Glyph::Play:
        pen.dl->PathLineTo(pen.p(3.5f, 2.0f));
        pen.dl->PathLineTo(pen.p(13.5f, 8.0f));
        pen.dl->PathLineTo(pen.p(3.5f, 14.0f));
        pen.dl->PathFillConvex(color);
        break;
    case Glyph::Stop: pen.fill_rect(3.0f, 3.0f, 13.0f, 13.0f, 1.0f); break;
    case Glyph::Build: draw_build(pen); break;
    case Glyph::Test: draw_test(pen); break;
    case Glyph::Plus:
        pen.line(8.0f, 2.5f, 8.0f, 13.5f);
        pen.line(2.5f, 8.0f, 13.5f, 8.0f);
        break;
    case Glyph::Search:
        pen.circle(6.75f, 6.75f, 4.75f);
        pen.dline(10.25f, 10.25f, 14.25f, 14.25f, 0, pen.t * 1.3f);
        break;
    case Glyph::Gear: draw_gear(pen); break;
    case Glyph::Link: draw_link(pen); break;
    case Glyph::Lock: draw_lock(pen, false); break;
    case Glyph::Unlock: draw_lock(pen, true); break;
    case Glyph::ExternalLink: draw_external_link(pen); break;
    case Glyph::Close:
        pen.dline(3.5f, 3.5f, 12.5f, 12.5f);
        pen.dline(12.5f, 3.5f, 3.5f, 12.5f);
        break;
    case Glyph::Folder: folder(pen); break;
    case Glyph::FolderOpen: folder_open(pen); break;
    case Glyph::File: page(pen); break;
    case Glyph::FileCode: draw_file_code(pen); break;
    case Glyph::Info: draw_info(pen); break;
    case Glyph::Warning: draw_warning(pen); break;
    case Glyph::Error: draw_error(pen); break;
    case Glyph::Check: pen.poly({{2.5f, 8.5f}, {6.25f, 12.25f}, {13.5f, 4.0f}}, false, false); break;
    case Glyph::Dot: pen.fill_circle(8.0f, 8.0f, 3.25f); break;
    }
}

void draw(ImDrawList* dl, ImVec2 min, float size, ComponentKind kind, ImU32 color) {
    if (!dl || size <= 0.0f) return;
    const Pen pen(dl, min, size, color);
    switch (kind) {
    case ComponentKind::Os: draw_os(pen); break;
    case ComponentKind::Group: folder(pen); break;
    case ComponentKind::Firmware: draw_firmware(pen); break;
    case ComponentKind::Cpu: draw_cpu(pen); break;
    case ComponentKind::Memory: draw_memory(pen); break;
    case ComponentKind::Interrupts: draw_interrupts(pen); break;
    case ComponentKind::Timer: draw_timer(pen); break;
    case ComponentKind::Clock: draw_clock(pen); break;
    case ComponentKind::Serial: draw_serial(pen); break;
    case ComponentKind::Display: draw_display(pen); break;
    case ComponentKind::Keyboard: draw_keyboard(pen); break;
    case ComponentKind::Mouse: draw_mouse(pen); break;
    case ComponentKind::Usb: draw_usb(pen); break;
    case ComponentKind::Storage: draw_storage(pen); break;
    case ComponentKind::Pci: draw_pci(pen); break;
    case ComponentKind::Network: draw_network(pen); break;
    case ComponentKind::Audio: draw_audio(pen); break;
    case ComponentKind::Power: draw_power(pen); break;
    case ComponentKind::Scheduler: draw_scheduler(pen); break;
    case ComponentKind::Processes: draw_processes(pen); break;
    case ComponentKind::Ipc: draw_ipc(pen); break;
    case ComponentKind::Syscalls: draw_syscalls(pen); break;
    case ComponentKind::FileSystem: draw_file_system(pen); break;
    case ComponentKind::Devices: draw_devices(pen); break;
    case ComponentKind::Shell: draw_shell(pen); break;
    case ComponentKind::Programs: draw_programs(pen); break;
    case ComponentKind::Program: draw_program(pen); break;
    case ComponentKind::Modules: draw_modules(pen); break;
    case ComponentKind::Module: draw_module(pen); break;
    }
}

ImU32 kind_color(ComponentKind kind) {
    switch (kind) {
    case ComponentKind::Os: return IM_COL32(0xE8, 0xE8, 0xE8, 0xFF);
    case ComponentKind::Group: return IM_COL32(0xB4, 0xB4, 0xB4, 0xFF);
    case ComponentKind::Firmware:
    case ComponentKind::Cpu:
    case ComponentKind::Memory:
    case ComponentKind::Interrupts:
    case ComponentKind::Timer:
    case ComponentKind::Clock:
    case ComponentKind::Serial:
    case ComponentKind::Display:
    case ComponentKind::Keyboard:
    case ComponentKind::Mouse:
    case ComponentKind::Usb:
    case ComponentKind::Storage:
    case ComponentKind::Pci:
    case ComponentKind::Network:
    case ComponentKind::Audio:
    case ComponentKind::Power: return IM_COL32(0x9C, 0xB4, 0xD0, 0xFF);   // hardware: blue-gray
    case ComponentKind::Scheduler:
    case ComponentKind::Processes:
    case ComponentKind::Ipc:
    case ComponentKind::Syscalls:
    case ComponentKind::FileSystem:
    case ComponentKind::Devices: return IM_COL32(0x6C, 0xC4, 0xB4, 0xFF);  // kernel: teal
    case ComponentKind::Shell:
    case ComponentKind::Programs:
    case ComponentKind::Program:
    case ComponentKind::Modules:
    case ComponentKind::Module: return IM_COL32(0xE0, 0xB4, 0x6A, 0xFF);   // userland: amber
    }
    return IM_COL32(0xD2, 0xD2, 0xD2, 0xFF);
}

void item(Glyph glyph, float size, ImU32 color) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    if (ImGui::IsItemVisible()) draw(ImGui::GetWindowDrawList(), pos, size, glyph, color);
}

void item(ComponentKind kind, float size, ImU32 color) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    if (ImGui::IsItemVisible()) draw(ImGui::GetWindowDrawList(), pos, size, kind, color);
}

} // namespace osef::editor::icons
