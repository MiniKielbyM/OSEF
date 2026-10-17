#pragma once

#include <vector>

struct GLFWwindow;

namespace osef::editor {

void set_window_icon(GLFWwindow* window);

// The app icon as size x size RGBA pixels (used for the macOS .icns too).
std::vector<unsigned char> render_icon(int size);

} // namespace osef::editor
