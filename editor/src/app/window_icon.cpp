#include "window_icon.h"

#include "util/png_reader.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdint>

#if defined(__APPLE__)
extern "C" void osef_editor_set_macos_app_icon(const unsigned char* pixels, int width, int height);
#endif

namespace osef::editor {
namespace {

constexpr int kIconSize = 64;  // design grid; render_icon() scales it to any size

struct Pixels {
    int size = kIconSize;
    float scale = 1.0f;
    std::vector<unsigned char> data;
    explicit Pixels(int s) : size(s), scale(static_cast<float>(s) / kIconSize), data(static_cast<size_t>(s) * s * 4) {}
};

struct Color {
    unsigned char r, g, b, a;
};

// Coordinates are in device pixels.
void set_pixel(Pixels& pixels, int x, int y, Color color) {
    if (x < 0 || x >= pixels.size || y < 0 || y >= pixels.size) return;
    const size_t i = static_cast<size_t>((y * pixels.size + x) * 4);
    pixels.data[i] = color.r;
    pixels.data[i + 1] = color.g;
    pixels.data[i + 2] = color.b;
    pixels.data[i + 3] = color.a;
}

// rounded_rect() and line() take coordinates on the 64-unit design grid.
void rounded_rect(Pixels& pixels, float l, float t, float r, float b, float rad, Color color) {
    const float k = pixels.scale;
    const int left = static_cast<int>(std::lround(l * k)), top = static_cast<int>(std::lround(t * k));
    const int right = static_cast<int>(std::lround(r * k)), bottom = static_cast<int>(std::lround(b * k));
    const float radius = rad * k;
    const float cx0 = left + radius;
    const float cx1 = right - radius;
    const float cy0 = top + radius;
    const float cy1 = bottom - radius;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = std::max({cx0 - x, 0.0f, x - cx1});
            const float dy = std::max({cy0 - y, 0.0f, y - cy1});
            if (dx * dx + dy * dy <= radius * radius) set_pixel(pixels, x, y, color);
        }
    }
}

void line(Pixels& pixels, float ax, float ay, float bx, float by, float width, Color color) {
    const float k = pixels.scale;
    const float x0 = ax * k, y0 = ay * k, x1 = bx * k, y1 = by * k, thickness = width * k;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length_sq = dx * dx + dy * dy;
    const int min_x = static_cast<int>(std::floor(std::min(x0, x1) - thickness));
    const int max_x = static_cast<int>(std::ceil(std::max(x0, x1) + thickness));
    const int min_y = static_cast<int>(std::floor(std::min(y0, y1) - thickness));
    const int max_y = static_cast<int>(std::ceil(std::max(y0, y1) + thickness));
    const float radius_sq = thickness * thickness * 0.25f;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float t = length_sq == 0.0f
                                ? 0.0f
                                : std::clamp(((x - x0) * dx + (y - y0) * dy) / length_sq, 0.0f, 1.0f);
            const float px = x0 + t * dx - x;
            const float py = y0 + t * dy - y;
            if (px * px + py * py <= radius_sq) set_pixel(pixels, x, y, color);
        }
    }
}

Pixels make_icon(int size) {
    Pixels pixels(size);
    rounded_rect(pixels, 0, 0, kIconSize, kIconSize, 13.0f, {24, 34, 47, 255});
    rounded_rect(pixels, 9, 11, 55, 53, 7.0f, {44, 62, 80, 255});
    rounded_rect(pixels, 13, 15, 51, 49, 4.0f, {20, 29, 41, 255});
    rounded_rect(pixels, 13, 15, 51, 25, 4.0f, {55, 75, 94, 255});
    rounded_rect(pixels, 13, 21, 51, 25, 0.0f, {55, 75, 94, 255});
    rounded_rect(pixels, 17, 18, 20, 21, 1.5f, {91, 207, 202, 255});
    rounded_rect(pixels, 22, 18, 25, 21, 1.5f, {115, 143, 168, 255});
    rounded_rect(pixels, 27, 18, 30, 21, 1.5f, {115, 143, 168, 255});

    constexpr Color cyan{93, 220, 215, 255};
    constexpr Color orange{255, 177, 92, 255};
    line(pixels, 29, 30, 23, 36, 3.5f, cyan);
    line(pixels, 23, 36, 29, 42, 3.5f, cyan);
    line(pixels, 35, 30, 41, 36, 3.5f, cyan);
    line(pixels, 41, 36, 35, 42, 3.5f, cyan);
    rounded_rect(pixels, 31, 31, 34, 42, 1.0f, orange);
    return pixels;
}

} // namespace

std::vector<unsigned char> render_icon(int size) { return make_icon(size).data; }

#include "app/icon_data.inc"  // kAppIconPng, generated from editor/assets/icon.png

namespace {
// The app icon from editor/assets/icon.png, or the drawn icon if that fails.
Pixels app_icon_pixels() {
    Image img;
    if (decode_png(kAppIconPng, sizeof(kAppIconPng), img, nullptr) && img.width == img.height) {
        Pixels pixels(img.width);
        pixels.data = std::move(img.rgba);
        return pixels;
    }
    return make_icon(256);
}
} // namespace

void set_window_icon(GLFWwindow* window) {
    static const Pixels pixels = app_icon_pixels();
#if defined(__APPLE__)
    (void)window;
    osef_editor_set_macos_app_icon(pixels.data.data(), pixels.size, pixels.size);
#else
    GLFWimage image{pixels.size, pixels.size, const_cast<unsigned char*>(pixels.data.data())};
    glfwSetWindowIcon(window, 1, &image);
#endif
}

} // namespace osef::editor
