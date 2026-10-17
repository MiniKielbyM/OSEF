#include "window_icon.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#if defined(__APPLE__)
extern "C" void osef_editor_set_macos_app_icon(const unsigned char* pixels, int width, int height);
#endif

namespace osef::editor {
namespace {

constexpr int kIconSize = 64;
using Pixels = std::array<unsigned char, kIconSize * kIconSize * 4>;

struct Color {
    unsigned char r, g, b, a;
};

void set_pixel(Pixels& pixels, int x, int y, Color color) {
    if (x < 0 || x >= kIconSize || y < 0 || y >= kIconSize) return;
    const size_t i = static_cast<size_t>((y * kIconSize + x) * 4);
    pixels[i] = color.r;
    pixels[i + 1] = color.g;
    pixels[i + 2] = color.b;
    pixels[i + 3] = color.a;
}

void rounded_rect(Pixels& pixels, int left, int top, int right, int bottom, float radius, Color color) {
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

void line(Pixels& pixels, float x0, float y0, float x1, float y1, float thickness, Color color) {
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

Pixels make_icon() {
    Pixels pixels{};
    rounded_rect(pixels, 0, 0, kIconSize, kIconSize, 13.0f, {24, 34, 47, 255});
    rounded_rect(pixels, 9, 11, 55, 53, 7.0f, {44, 62, 80, 255});
    rounded_rect(pixels, 13, 15, 51, 49, 4.0f, {20, 29, 41, 255});
    rounded_rect(pixels, 13, 15, 51, 25, 4.0f, {55, 75, 94, 255});
    for (int y = 21; y < 25; ++y)
        for (int x = 13; x < 51; ++x) set_pixel(pixels, x, y, {55, 75, 94, 255});
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

void set_window_icon(GLFWwindow* window) {
    static const Pixels pixels = make_icon();
#if defined(__APPLE__)
    (void)window;
    osef_editor_set_macos_app_icon(pixels.data(), kIconSize, kIconSize);
#else
    GLFWimage image{kIconSize, kIconSize, const_cast<unsigned char*>(pixels.data())};
    glfwSetWindowIcon(window, 1, &image);
#endif
}

} // namespace osef::editor
