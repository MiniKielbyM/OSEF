#pragma once
// App icon assets generated from one source image (editor/assets/icon.png):
// a Windows .ico, a macOS .iconset (for iconutil -> .icns) and a C++ include
// with a 256 px PNG that the app decodes at runtime for its window/Dock icon.

#include "util/png_reader.h"

#include <filesystem>
#include <string>

namespace osef::editor {

// Windows icon with 16, 24, 32, 48, 64, 128 and 256 px PNG entries.
bool write_ico(const std::filesystem::path& path, const Image& master, std::string* error);
// macOS .iconset directory: icon_{16,32,128,256,512}x..(@2x).png.
bool write_iconset(const std::filesystem::path& dir, const Image& master, std::string* error);
// `static const unsigned char kAppIconPng[] = {...};` with a 256 px PNG.
bool write_icon_include(const std::filesystem::path& path, const Image& master, std::string* error);

} // namespace osef::editor
