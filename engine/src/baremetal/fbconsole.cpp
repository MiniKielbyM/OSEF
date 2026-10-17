// Framebuffer text console: 8x8 public-domain font (2x scaled on framebuffers
// >= 1600 px wide), 32 bpp, colours built from Limine's mask size/shift
// fields, a small ANSI subset and scrolling by moving pixel rows.
// Needs no heap, so the panic path can bring it up at any time.

#include "font8x8.h"
#include "internal.h"
#include "limine.h"

namespace osef::bm {

namespace {

struct Rgb { uint8_t r, g, b; };
// VGA-style palette: 0-7 normal, 8-15 bright.
constexpr Rgb kPalette[16] = {
    {0, 0, 0},       {170, 0, 0},     {0, 170, 0},     {170, 85, 0},
    {0, 0, 170},     {170, 0, 170},   {0, 170, 170},   {170, 170, 170},
    {85, 85, 85},    {255, 85, 85},   {85, 255, 85},   {255, 255, 85},
    {85, 85, 255},   {255, 85, 255},  {85, 255, 255},  {255, 255, 255},
};
constexpr int kDefaultFg = 7;
constexpr int kDefaultBg = 0;

struct Console {
    bool active = false;
    uint8_t* fb = nullptr;
    uint64_t width = 0, height = 0, pitch = 0;
    uint8_t rsize = 8, rshift = 16, gsize = 8, gshift = 8, bsize = 8, bshift = 0;
    uint32_t scale = 1, cell = 8;
    uint32_t cols = 0, rows = 0;
    uint32_t cx = 0, cy = 0;
    int fg = kDefaultFg, bg = kDefaultBg;
    bool bold = false;
    uint32_t pixel[16] = {};
    bool cursor_shown = false;
    // ANSI parser
    enum class State { Normal, Esc, Csi } state = State::Normal;
    uint32_t params[8] = {};
    uint32_t nparams = 0;
    bool param_seen = false;
};

Console g_con;

uint32_t channel(uint8_t v, uint8_t size, uint8_t shift) {
    if (size == 0) return 0;
    uint32_t c = size >= 8 ? (static_cast<uint32_t>(v) << (size - 8)) : (static_cast<uint32_t>(v) >> (8 - size));
    return c << shift;
}

uint32_t make_pixel(Rgb c) {
    return channel(c.r, g_con.rsize, g_con.rshift) | channel(c.g, g_con.gsize, g_con.gshift) |
           channel(c.b, g_con.bsize, g_con.bshift);
}

inline uint32_t* row_ptr(uint64_t y) { return reinterpret_cast<uint32_t*>(g_con.fb + y * g_con.pitch); }

void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t px) {
    for (uint64_t j = 0; j < h; ++j) {
        volatile uint32_t* row = row_ptr(y + j) + x;
        for (uint64_t i = 0; i < w; ++i) row[i] = px;
    }
}

int effective_fg() { return (g_con.bold && g_con.fg < 8) ? g_con.fg + 8 : g_con.fg; }

void draw_glyph(uint32_t col, uint32_t row, char ch) {
    uint8_t c = static_cast<uint8_t>(ch);
    if (c >= 128) c = '?';
    const uint8_t* glyph = font8x8_basic[c];
    uint32_t fgp = g_con.pixel[effective_fg()];
    uint32_t bgp = g_con.pixel[g_con.bg];
    uint32_t s = g_con.scale;
    uint64_t x0 = static_cast<uint64_t>(col) * g_con.cell;
    uint64_t y0 = static_cast<uint64_t>(row) * g_con.cell;
    for (uint32_t gy = 0; gy < 8; ++gy) {
        uint8_t bits = glyph[gy];
        for (uint32_t sy = 0; sy < s; ++sy) {
            volatile uint32_t* p = row_ptr(y0 + gy * s + sy) + x0;
            for (uint32_t gx = 0; gx < 8; ++gx) {
                uint32_t px = (bits >> gx) & 1 ? fgp : bgp;
                for (uint32_t sx = 0; sx < s; ++sx) *p++ = px;
            }
        }
    }
}

// The cursor is an underline drawn by inverting pixels, so toggling it twice
// restores the cell exactly (no character buffer needed).
void toggle_cursor() {
    if (g_con.cx >= g_con.cols || g_con.cy >= g_con.rows) return;
    uint32_t mask = make_pixel({255, 255, 255});
    uint32_t h = g_con.scale * 1;
    uint64_t x0 = static_cast<uint64_t>(g_con.cx) * g_con.cell;
    uint64_t y0 = static_cast<uint64_t>(g_con.cy) * g_con.cell + g_con.cell - h;
    for (uint32_t j = 0; j < h; ++j) {
        volatile uint32_t* p = row_ptr(y0 + j) + x0;
        for (uint32_t i = 0; i < g_con.cell; ++i) p[i] = p[i] ^ mask;
    }
}

void hide_cursor() {
    if (g_con.cursor_shown) {
        toggle_cursor();
        g_con.cursor_shown = false;
    }
}
void show_cursor() {
    if (!g_con.cursor_shown) {
        toggle_cursor();
        g_con.cursor_shown = true;
    }
}

void clear_cells(uint32_t col0, uint32_t row0, uint32_t col1, uint32_t row1) {  // [col0,col1) x [row0,row1)
    if (col1 <= col0 || row1 <= row0) return;
    fill_rect(static_cast<uint64_t>(col0) * g_con.cell, static_cast<uint64_t>(row0) * g_con.cell,
              static_cast<uint64_t>(col1 - col0) * g_con.cell, static_cast<uint64_t>(row1 - row0) * g_con.cell,
              g_con.pixel[g_con.bg]);
}

void scroll_up() {
    uint64_t text_h = static_cast<uint64_t>(g_con.rows) * g_con.cell;
    uint64_t shift = g_con.cell;
    uint64_t row_bytes = g_con.width * 4;
    for (uint64_t y = 0; y + shift < text_h; ++y) {
        const volatile uint64_t* src = reinterpret_cast<const uint64_t*>(row_ptr(y + shift));
        volatile uint64_t* dst = reinterpret_cast<uint64_t*>(row_ptr(y));
        uint64_t n = row_bytes / 8;
        for (uint64_t i = 0; i < n; ++i) dst[i] = src[i];
        if (row_bytes & 4)
            reinterpret_cast<volatile uint32_t*>(dst)[n * 2] = reinterpret_cast<const volatile uint32_t*>(src)[n * 2];
    }
    clear_cells(0, g_con.rows - 1, g_con.cols, g_con.rows);
}

void newline() {
    g_con.cx = 0;
    if (g_con.cy + 1 >= g_con.rows) scroll_up();
    else ++g_con.cy;
}

void put_printable(char c) {
    if (g_con.cx >= g_con.cols) newline();  // deferred wrap
    draw_glyph(g_con.cx, g_con.cy, c);
    ++g_con.cx;
}

uint32_t param(uint32_t i, uint32_t def) {
    return (i < g_con.nparams && g_con.params[i] != 0) ? g_con.params[i] : def;
}

void apply_sgr() {
    if (g_con.nparams == 0) {
        g_con.fg = kDefaultFg;
        g_con.bg = kDefaultBg;
        g_con.bold = false;
        return;
    }
    for (uint32_t i = 0; i < g_con.nparams; ++i) {
        uint32_t p = g_con.params[i];
        if (p == 0) {
            g_con.fg = kDefaultFg;
            g_con.bg = kDefaultBg;
            g_con.bold = false;
        } else if (p == 1) g_con.bold = true;
        else if (p == 22) g_con.bold = false;
        else if (p >= 30 && p <= 37) g_con.fg = static_cast<int>(p - 30);
        else if (p == 39) g_con.fg = kDefaultFg;
        else if (p >= 40 && p <= 47) g_con.bg = static_cast<int>(p - 40);
        else if (p == 49) g_con.bg = kDefaultBg;
        else if (p >= 90 && p <= 97) g_con.fg = static_cast<int>(p - 90 + 8);
        else if (p >= 100 && p <= 107) g_con.bg = static_cast<int>(p - 100 + 8);
    }
}

void csi_final(char f) {
    switch (f) {
    case 'm': apply_sgr(); break;
    case 'J': {
        uint32_t mode = g_con.nparams ? g_con.params[0] : 0;
        if (mode == 2 || mode == 3) {
            clear_cells(0, 0, g_con.cols, g_con.rows);
        } else if (mode == 0) {
            clear_cells(g_con.cx, g_con.cy, g_con.cols, g_con.cy + 1);
            clear_cells(0, g_con.cy + 1, g_con.cols, g_con.rows);
        } else if (mode == 1) {
            clear_cells(0, 0, g_con.cols, g_con.cy);
            clear_cells(0, g_con.cy, g_con.cx + 1 < g_con.cols ? g_con.cx + 1 : g_con.cols, g_con.cy + 1);
        }
        break;
    }
    case 'K': {
        uint32_t mode = g_con.nparams ? g_con.params[0] : 0;
        uint32_t cx = g_con.cx < g_con.cols ? g_con.cx : g_con.cols;
        if (mode == 0) clear_cells(cx, g_con.cy, g_con.cols, g_con.cy + 1);
        else if (mode == 1) clear_cells(0, g_con.cy, cx + 1 < g_con.cols ? cx + 1 : g_con.cols, g_con.cy + 1);
        else if (mode == 2) clear_cells(0, g_con.cy, g_con.cols, g_con.cy + 1);
        break;
    }
    case 'H':
    case 'f': {
        uint32_t r = param(0, 1), c = param(1, 1);
        g_con.cy = (r - 1 < g_con.rows) ? r - 1 : g_con.rows - 1;
        g_con.cx = (c - 1 < g_con.cols) ? c - 1 : g_con.cols - 1;
        break;
    }
    case 'A': {
        uint32_t n = param(0, 1);
        g_con.cy = n > g_con.cy ? 0 : g_con.cy - n;
        break;
    }
    case 'B': {
        uint32_t n = param(0, 1);
        g_con.cy = (g_con.cy + n >= g_con.rows) ? g_con.rows - 1 : g_con.cy + n;
        break;
    }
    case 'C': {
        uint32_t n = param(0, 1);
        g_con.cx = (g_con.cx + n >= g_con.cols) ? g_con.cols - 1 : g_con.cx + n;
        break;
    }
    case 'D': {
        uint32_t n = param(0, 1);
        if (g_con.cx > g_con.cols - 1) g_con.cx = g_con.cols - 1;
        g_con.cx = n > g_con.cx ? 0 : g_con.cx - n;
        break;
    }
    default: break;  // unsupported sequences are swallowed
    }
}

void put_char(char c) {
    switch (g_con.state) {
    case Console::State::Esc:
        if (c == '[') {
            g_con.state = Console::State::Csi;
            g_con.nparams = 0;
            g_con.param_seen = false;
            for (auto& p : g_con.params) p = 0;
        } else {
            g_con.state = Console::State::Normal;  // other escapes ignored
        }
        return;
    case Console::State::Csi:
        if (c >= '0' && c <= '9') {
            if (!g_con.param_seen) {
                if (g_con.nparams < 8) ++g_con.nparams;
                g_con.param_seen = true;
            }
            uint32_t& p = g_con.params[g_con.nparams - 1];
            if (p < 100000) p = p * 10 + static_cast<uint32_t>(c - '0');
        } else if (c == ';') {
            if (!g_con.param_seen && g_con.nparams < 8) ++g_con.nparams;  // empty parameter = 0
            g_con.param_seen = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            g_con.state = Console::State::Normal;
            csi_final(c);
        } else if (c == '?' || c == '>' || c == ' ') {
            // private / intermediate bytes: accepted, ignored
        } else {
            g_con.state = Console::State::Normal;
        }
        return;
    case Console::State::Normal: break;
    }
    switch (c) {
    case '\x1b': g_con.state = Console::State::Esc; break;
    case '\r': g_con.cx = 0; break;
    case '\n': newline(); break;
    case '\b':
        if (g_con.cx >= g_con.cols) g_con.cx = g_con.cols - 1;
        else if (g_con.cx > 0) --g_con.cx;
        break;
    case '\t': {
        uint32_t next = (g_con.cx + 8) & ~7u;
        if (next >= g_con.cols) next = g_con.cols;
        while (g_con.cx < next) put_printable(' ');
        break;
    }
    case '\a': break;
    default:
        if (static_cast<uint8_t>(c) >= 0x20) put_printable(c);
        break;
    }
}

} // namespace

bool fbcon_init(const limine_framebuffer* fb) {
    if (!fb || !fb->address || fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return false;
    if (fb->width < 64 || fb->height < 64) return false;
    g_con = Console{};
    g_con.fb = static_cast<uint8_t*>(fb->address);
    g_con.width = fb->width;
    g_con.height = fb->height;
    g_con.pitch = fb->pitch;
    g_con.rsize = fb->red_mask_size;
    g_con.rshift = fb->red_mask_shift;
    g_con.gsize = fb->green_mask_size;
    g_con.gshift = fb->green_mask_shift;
    g_con.bsize = fb->blue_mask_size;
    g_con.bshift = fb->blue_mask_shift;
    g_con.scale = fb->width >= 1600 ? 2 : 1;
    g_con.cell = 8 * g_con.scale;
    g_con.cols = static_cast<uint32_t>(fb->width / g_con.cell);
    g_con.rows = static_cast<uint32_t>(fb->height / g_con.cell);
    for (int i = 0; i < 16; ++i) g_con.pixel[i] = make_pixel(kPalette[i]);
    fill_rect(0, 0, g_con.width, g_con.height, g_con.pixel[kDefaultBg]);
    g_con.active = true;
    show_cursor();
    return true;
}

bool fbcon_active() { return g_con.active; }

void fbcon_write(const char* s, size_t n) {
    if (!g_con.active) return;
    hide_cursor();
    for (size_t i = 0; i < n; ++i) put_char(s[i]);
    show_cursor();
}

} // namespace osef::bm
